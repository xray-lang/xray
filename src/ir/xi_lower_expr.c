/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_expr.c - Expression lowering (extracted from xi_lower.c)
 *
 * Contains: type inference helpers, all lower_* expression functions,
 * and the xi_lower_expr() dispatch switch.
 */

#include "xi_lower_internal.h"
#include "xi_semantic_intrinsic.h"
#include "xi.h"
#include "xi_effect.h"
#include "xi_lower_expr_helpers.h"
#include "xi_own.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../shared/xr_scalar_type.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/parser/xtype_ref.h"
#include "../analysis/xglobal_summary.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xanalyzer_builtins.h"
#include "../frontend/analyzer/xa_parallel_call_plan.h"
#include "../frontend/analyzer/xa_typed_program.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../frontend/analyzer/xa_selection.h"
#include "../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../frontend/analyzer/xconsteval.h"
#include "../frontend/lexer/xlex.h"
#include "../runtime/class/xclass_system.h"

#include "../module/xmodule_graph.h"
#include "../runtime/class/xenum.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/object/xstring.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"
#include "../shared/xr_array_core.h"
#include "../../stdlib/stdlib_cache.h"
#include "../shared/xr_elem_type.h"
#include "../base/xglobal_indices.h"
#include "../base/xconstants.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/value/xffi_sig.h"
#include "../shared/xr_encoding_constants.h"

#include <string.h>
#include <stdio.h>
#include <float.h>
#include <math.h>
#include <limits.h>

/* ========== Forward Declarations ========== */

static XiValue *lower_try_construct_call(XiLower *l, AstNode *node, CallExprNode *call);
static XiValue *lower_construct(XiLower *l, AstNode *node, struct XrType *result_type,
                                const char *module_name, const char *cname, AstNode **arguments,
                                XrCallArgAccess *arg_accesses, int arg_count);
static XiValue *lower_parallel_module_intrinsic_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                     XaParallelCallKind kind);
static XiValue *lower_parallel_plan_intrinsic_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                   XiValue *plan, XaParallelCallKind kind,
                                                   XaIntrinsicId intrinsic_id);

static int pack_go_aux(int link_mode) {
    return link_mode & XI_GO_AUX_LINK_MASK;
}

static int64_t pack_thread_spawn_aux(int64_t stack_size) {
    if (stack_size <= 0)
        return 0;
    int64_t masked = stack_size & XI_THREAD_SPAWN_AUX_STACK_SIZE_MASK;
    return masked << XI_THREAD_SPAWN_AUX_STACK_SIZE_SHIFT;
}

static int xi_lower_builtin_class_global_index(const char *name) {
    if (!name)
        return -1;
    static const struct {
        const char *name;
        int index;
    } builtin_classes[] = {
        {"Array", XR_GLOBAL_VAR_ARRAY},
        {"Set", XR_GLOBAL_VAR_SET},
        {"Map", XR_GLOBAL_VAR_MAP},
        {"String", XR_GLOBAL_VAR_STRING},
        {"Json", XR_GLOBAL_VAR_JSON},
        {"Process", XR_GLOBAL_VAR_PROCESS},
        {"PanicInfo", XR_GLOBAL_VAR_PANIC_INFO},
        {"Range", XR_GLOBAL_VAR_RANGE},
        {"Atomic", XR_GLOBAL_VAR_ATOMIC},
        {"Ordering", XR_GLOBAL_VAR_ORDERING},
        {"Endian", XR_GLOBAL_VAR_ENDIAN},
        {"Recv", XR_GLOBAL_VAR_RECV},
        {"SendResult", XR_GLOBAL_VAR_SEND_RESULT},
        {"TaskResult", XR_GLOBAL_VAR_TASK_RESULT},
        {"TaskStatus", XR_GLOBAL_VAR_TASK_STATUS},
        {"Utf8Error", XR_GLOBAL_VAR_UTF8_ERROR},
        {"StringSliceError", XR_GLOBAL_VAR_STRING_SLICE_ERROR},
        {"CompressionError", XR_GLOBAL_VAR_COMPRESSION_ERROR},
        {"CryptoError", XR_GLOBAL_VAR_CRYPTO_ERROR},
        {"WorkQueue", XR_GLOBAL_VAR_WORKQUEUE},
        {"ResultGroup", XR_GLOBAL_VAR_RESULTGROUP},
        {"CountdownLatch", XR_GLOBAL_VAR_COUNTDOWNLATCH},
        {"Semaphore", XR_GLOBAL_VAR_SEMAPHORE},
        {"EventCount", XR_GLOBAL_VAR_EVENTCOUNT},
    };
    for (int i = 0; i < (int) (sizeof(builtin_classes) / sizeof(builtin_classes[0])); i++) {
        if (strcmp(name, builtin_classes[i].name) == 0)
            return builtin_classes[i].index;
    }
    return -1;
}

static int xi_lower_type_constant_id(const char *name) {
    return xr_type_from_name(name);
}

static XiValue *xi_lower_emit_builtin_class(XiLower *l, const char *name, int line) {
    int index = xi_lower_builtin_class_global_index(name);
    if (index < 0)
        return NULL;
    struct XrType *cls_type = xr_type_new_class(NULL, name);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, cls_type, 0);
    if (v) {
        v->aux_int = index;
        v->aux = (void *) arena_strdup(l->func, name);
        v->line = (uint32_t) line;
    }
    return v;
}

static bool xi_lower_sync_runtime_class_name(const char *name) {
    return name && (strcmp(name, "Semaphore") == 0 || strcmp(name, "CountdownLatch") == 0 ||
                    strcmp(name, "EventCount") == 0 || strcmp(name, "WorkQueue") == 0 ||
                    strcmp(name, "ResultGroup") == 0);
}

static bool xi_lower_symbol_is_sync_runtime_class(XiLower *l, uint32_t sid, const char *name) {
    if (!l || !l->analyzer || !sid)
        return false;
    XaSymbol *sym = xa_scope_lookup_by_id(l->analyzer->global_scope, sid);
    if (!sym || (sym->kind != XA_SYM_CLASS && sym->kind != XA_SYM_IMPORT))
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    if (!links)
        return false;
    const char *class_name = links->import_member_name ? links->import_member_name : name;
    if (!xi_lower_sync_runtime_class_name(class_name))
        return false;
    if (links->module_name && strcmp(links->module_name, "sync") == 0)
        return true;
    return links->file_path && strstr(links->file_path, "stdlib/sync/sync.xr") != NULL;
}

static XaSymbol *xi_lower_lookup_class_symbol(XiLower *l, const char *name) {
    if (!l || !l->analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(l->analyzer, name);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_in_scope(l->analyzer, name, l->analyzer->global_scope);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_deep(l->analyzer, name);
    return (sym && sym->kind == XA_SYM_CLASS) ? sym : NULL;
}

static XrAggregateLayout *xi_lower_lookup_struct_layout(XiLower *l, const char *name) {
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return (links && links->class_info) ? links->class_info->struct_layout : NULL;
}

static XrClassInfo *xi_lower_lookup_class_info(XiLower *l, const char *name) {
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return links ? links->class_info : NULL;
}

static struct XrType *xi_lower_class_info_constructor_type(XiLower *l, XrClassInfo *class_info) {
    if (!l || !l->analyzer || !class_info)
        return NULL;
    XaSymbol *ctor =
        class_info ? xa_class_info_lookup_member(class_info, XR_KEYWORD_CONSTRUCTOR) : NULL;
    XaSymbolLinks *ctor_links = ctor ? xa_analyzer_get_links(l->analyzer, ctor) : NULL;
    struct XrType *ctor_type = ctor_links ? ctor_links->type : NULL;
    return ctor_type && ctor_type->kind == XR_KIND_FUNCTION ? ctor_type : NULL;
}

static struct XrType *xi_lower_class_constructor_type(XiLower *l, XaSymbol *class_sym) {
    if (!l || !l->analyzer || !class_sym || class_sym->kind != XA_SYM_CLASS)
        return NULL;
    XaSymbolLinks *class_links = xa_analyzer_get_links(l->analyzer, class_sym);
    return xi_lower_class_info_constructor_type(l, class_links ? class_links->class_info : NULL);
}

static struct XrType *xi_lower_type_constructor_type(XiLower *l, struct XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE))
        return NULL;
    XrClassInfo *class_info = type->instance.class_ref;
    if (!class_info && type->instance.class_name)
        class_info = xi_lower_lookup_class_info(l, type->instance.class_name);
    return xi_lower_class_info_constructor_type(l, class_info);
}

static struct XrType *xi_lower_call_constructor_type(XiLower *l, const CallExprNode *call) {
    if (!l || !l->analyzer || !call || !call->callee || call->callee->type != AST_VARIABLE)
        return NULL;
    VariableNode *callee = &call->callee->as.variable;
    XaSymbol *class_sym = callee->symbol_id
                              ? xa_scope_lookup_by_id(l->analyzer->global_scope, callee->symbol_id)
                              : NULL;
    if (!class_sym && callee->name)
        class_sym = xi_lower_lookup_class_symbol(l, callee->name);
    return xi_lower_class_constructor_type(l, class_sym);
}

/* Does `T(args)` construct a user class instance?  The fact is about the
 * callee's symbol kind, not about the class declaring an explicit constructor:
 * a class without one gets a synthesized constructor during class lowering and
 * the call still allocates a fresh instance (xr_instance_new).  Requiring an
 * XrClassInfo excludes the builtin classes, which dispatch through a `call`
 * static method that may hand back an existing object.
 *
 * ARC consumes the resulting XI_LOWERING_FLAG_CONSTRUCTOR_CALL to own and drop
 * the result; a false positive would be a double release, so every uncertain
 * shape answers false and keeps the alias-uncertain treatment. */
static bool xi_lower_call_constructs_instance(XiLower *l, const CallExprNode *call,
                                              const struct XrType *result_type) {
    if (!l || !l->analyzer || !call || !call->callee || call->callee->type != AST_VARIABLE)
        return false;
    /* Constructing yields an instance of the constructed class. */
    if (!result_type || result_type->kind != XR_KIND_INSTANCE)
        return false;
    const VariableNode *callee = &call->callee->as.variable;
    XaSymbol *class_sym = NULL;
    if (callee->symbol_id) {
        /* A callee id resolving to something other than a class (a local that
         * shadows the class name) is not a construction.  Deliberately no name
         * fallback here: that would look straight past the shadowing. */
        XaSymbol *sym = xa_scope_lookup_by_id(l->analyzer->global_scope, callee->symbol_id);
        if (sym && sym->kind == XA_SYM_CLASS)
            class_sym = sym;
    } else if (callee->name) {
        class_sym = xi_lower_lookup_class_symbol(l, callee->name);
    }
    if (!class_sym)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, class_sym);
    return links && links->class_info != NULL;
}

static XiValue *xi_lower_emit_import_ref(XiLower *l, const char *module_name,
                                         const char *member_name, struct XrType *type, int line) {
    if (!l || !module_name)
        return NULL;

    XiImportRef *ref = (XiImportRef *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiImportRef));
    if (!ref)
        return NULL;
    memset(ref, 0, sizeof(*ref));

    uint32_t ml = (uint32_t) strlen(module_name);
    char *mc = (char *) xi_func_arena_alloc(l->func, ml + 1);
    if (!mc)
        return NULL;
    memcpy(mc, module_name, ml + 1);
    ref->module_path = mc;

    if (member_name) {
        uint32_t nl = (uint32_t) strlen(member_name);
        char *nc = (char *) xi_func_arena_alloc(l->func, nl + 1);
        if (!nc)
            return NULL;
        memcpy(nc, member_name, nl + 1);
        ref->member_name = nc;
    }

    ref->resolved_mod_index = -1;
    ref->resolved_shared_slot = -1;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_IMPORT_REF, type ? type : l->type_any, 0);
    if (!v)
        return NULL;
    v->aux = (void *) ref;
    v->aux_int = -1;
    v->line = (uint32_t) line;
    return v;
}

static XiValue *xi_lower_builtin_module_function_ref(XiLower *l, uint32_t sid,
                                                     const char *fallback_name, int line) {
    if (!l || !l->analyzer || sid == 0)
        return NULL;

    XaSymbol *sym = xa_scope_lookup_by_id(l->analyzer->global_scope, sid);
    if (!sym || !sym->is_builtin || sym->kind != XA_SYM_FUNCTION)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    const char *module_name = links ? links->module_name : NULL;
    const char *member_name =
        links && links->import_member_name ? links->import_member_name : fallback_name;
    if (!module_name || !member_name)
        return NULL;

    return xi_lower_emit_import_ref(l, module_name, member_name, links->type, line);
}

static const char *xi_lower_export_module_for_symbol(XiLower *l, XaSymbol *target,
                                                     const char *export_name) {
    if (!l || !l->analyzer || !target || !export_name)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, target);
    if (links && links->module_name)
        return links->module_name;

    XrModuleGraph *graph = (XrModuleGraph *) l->analyzer->graph;
    if (!graph)
        return NULL;

    for (int i = 0; i < graph->spec_count; i++) {
        XrModuleSpec *spec = &graph->specs[i];
        XaSymbol *candidate = spec->export_symbols
                                  ? (XaSymbol *) xr_hashmap_get(spec->export_symbols, export_name)
                                  : NULL;
        if (!candidate)
            continue;
        if (candidate == target || (candidate->id != 0 && candidate->id == target->id))
            return spec->canonical;
    }
    return NULL;
}

XiValue *xi_lower_enum_namespace_value(XiLower *l, XaSymbol *enum_sym, const char *enum_name,
                                       int line) {
    if (!l || !enum_sym || !enum_name)
        return NULL;

    int builtin_idx = xi_lower_builtin_class_global_index(enum_name);
    if (builtin_idx >= 0)
        return xi_lower_emit_builtin_class(l, enum_name, line);

    const char *module_path = xi_lower_export_module_for_symbol(l, enum_sym, enum_name);
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, enum_sym);
    const XaBuiltinEnum *native_decl =
        module_path ? xa_builtin_get_enum_type(module_path, enum_name) : NULL;
    XaEnumInfo *info = links ? links->enum_info : NULL;
    if (native_decl && info && info->variant_count > 0) {
        XrEnumType *runtime_type = xr_stdlib_enum_type_get(l->isolate, module_path, enum_name);
        XiEnumData *data = (XiEnumData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(*data));
        XiEnumMemberData *members = (XiEnumMemberData *) xi_func_arena_alloc(
            l->func, (uint32_t) (sizeof(*members) * info->variant_count));
        if (!runtime_type || !links->type || !data || !members)
            return NULL;
        memset(data, 0, sizeof(*data));
        memset(members, 0, sizeof(*members) * info->variant_count);
        data->name = arena_strdup(l->func, enum_name);
        data->member_count = info->variant_count;
        data->is_adt = info->is_payload_enum;
        data->layout_id = native_decl->layout_id;
        data->runtime_type = runtime_type;
        data->members = members;
        for (uint32_t i = 0; i < info->variant_count; i++) {
            const XaEnumVariantInfo *variant = &info->variants[i];
            members[i].name = arena_strdup(l->func, variant->name);
            members[i].ordinal = variant->tag;
            members[i].payload_count = (int) variant->payload_count;
            members[i].payload_types = variant->payload_types;
            if (members[i].payload_count > data->max_payload)
                data->max_payload = members[i].payload_count;
        }
        XiValue *value = xi_value_new(l->func, l->cur_block, XI_CONST, links->type, 0);
        if (!value)
            return NULL;
        value->aux = data;
        value->aux_kind = XI_AUX_KIND_ENUM_NAMESPACE;
        value->line = (uint32_t) line;
        return value;
    }

    int var_id = xi_lower_var_find(l, enum_sym->id, enum_name);
    if (var_id >= 0)
        return xi_lower_braun_read(l, var_id, l->cur_block);

    XiTopBinding top = xi_lower_find_top_binding(l, enum_sym->id, enum_name);
    if (xi_top_binding_valid(top))
        return xi_lower_emit_top_load(l, top, NULL);

    if (!module_path)
        return NULL;
    return xi_lower_emit_import_ref(l, module_path, enum_name, links ? links->type : l->type_any,
                                    line);
}

XR_FUNC XrAggregateLayout *xi_lower_type_struct_layout(XiLower *l, struct XrType *type) {
    XrAggregateLayout *layout = xi_lower_struct_layout_of(type);
    if (layout)
        return layout;
    if (!type)
        return NULL;
    const char *class_name = xr_type_get_class_name(type);
    return class_name ? xi_lower_lookup_struct_layout(l, class_name) : NULL;
}

static bool xi_lower_type_is_named_instance(const XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_lower_type_is_named_instance(type->union_type.members[i], name))
                return true;
        }
    }
    return false;
}

static bool xi_lower_method_may_suspend(const XrType *receiver_type, const char *method,
                                        int nargs) {
    if (!receiver_type || !method)
        return false;
    if (xi_lower_type_is_named_instance(receiver_type, "WorkQueue"))
        return strcmp(method, "pop") == 0 && (nargs == 0 || nargs == 1);
    if (xi_lower_type_is_named_instance(receiver_type, "ResultGroup"))
        return strcmp(method, "recv") == 0 && nargs == 0;
    return false;
}

static XrAggregateLayout *xi_lower_value_struct_layout(XiLower *l, XiValue *v) {
    XrAggregateLayout *layout = xi_lower_type_struct_layout(l, v ? v->type : NULL);
    if (layout)
        return layout;
    while (v && (xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op)) && v->nargs >= 1)
        v = v->args[0];
    layout = xi_lower_type_struct_layout(l, v ? v->type : NULL);
    if (layout)
        return layout;
    if (!v || v->op != XI_AGG_GET)
        return NULL;
    XrAggregateLayout *parent = (XrAggregateLayout *) v->aux;
    if (!parent || v->aux_int < 0 || v->aux_int >= parent->field_count)
        return NULL;
    XrAggregateFieldLayout *field = &parent->fields[v->aux_int];
    return field->native_type == XR_NATIVE_NESTED_AGGREGATE ? field->sub_layout : NULL;
}

static bool xi_lower_type_needs_value_clone(XiLower *l, struct XrType *type) {
    return type && (type->kind == XR_KIND_FIXED_ARRAY || type->is_value_type ||
                    xi_lower_type_struct_layout(l, type) != NULL);
}

XR_FUNC bool xi_lower_type_uses_read_place(XiLower *l, struct XrType *type) {
    if (!type || type->is_nullable)
        return false;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return true;
    return (type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) &&
           xi_lower_type_struct_layout(l, type) != NULL;
}

static bool xi_lower_value_needs_value_clone(XiLower *l, XiValue *v) {
    return v && xi_lower_type_needs_value_clone(l, v->type);
}

static bool xi_lower_value_is_fresh_value_struct(XiValue *v) {
    if (!v || xi_var_id_is_valid(v->var_id))
        return false;
    return v->op == XI_AGG_NEW || v->op == XI_FIXED_ARRAY_NEW || v->op == XI_FIXED_BYTES_CONST ||
           (v->op == XI_COPY && v->aux_int == XI_COPY_KIND_VALUE_CLONE);
}

static void xi_lower_mark_value_clone_copy(XiValue *v) {
    if (v && v->op == XI_COPY)
        v->aux_int = XI_COPY_KIND_VALUE_CLONE;
}

static XiValue *xi_lower_apply_primitive_type_view(XiLower *l, AstNode *node, XiValue *val,
                                                   struct XrType *target_type) {
    if (!l || !node || !val || !val->type || !target_type || xr_type_equals(val->type, target_type))
        return val;
    if (XR_TYPE_IS_FLOAT(val->type) && XR_TYPE_IS_FLOAT(target_type) &&
        target_type->scalar_rep == XR_NATIVE_F32) {
        XiValue *n = xi_value_new(l->func, l->cur_block, XI_NARROW_F32, target_type, 1);
        if (!n)
            return val;
        n->args[0] = val;
        n->line = (uint32_t) node->line;
        return n;
    }
    if (!((XR_TYPE_IS_INT(val->type) && XR_TYPE_IS_INT(target_type)) ||
          (XR_TYPE_IS_FLOAT(val->type) && XR_TYPE_IS_FLOAT(target_type))))
        return val;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, target_type, 1);
    if (!copy)
        return val;
    copy->args[0] = val;
    copy->line = (uint32_t) node->line;
    return copy;
}

XR_FUNC XiValue *xi_lower_checktype_for_type(XiLower *l, AstNode *node, XiValue *val,
                                             struct XrType *target_type) {
    if (!l || !l->func || !node || !val || !target_type || XR_TYPE_IS_UNKNOWN(target_type))
        return val;
    if (val->type && xr_type_assignable(target_type, val->type))
        return xi_lower_apply_primitive_type_view(l, node, val, target_type);

    /* `T?` reaching a `T` target is a narrowing the analyzer already proved:
     * it rejects the unguarded form outright ("cannot assign 'T?' to 'T'
     * without null check"), so the only thing a dynamic check could catch here
     * has been excluded statically.  Emitting one anyway is not merely wasted
     * work -- it keeps the value in tagged form and blocks the unboxed
     * representation the narrowing exists to enable. */
    if (val->type && val->type->is_nullable && !target_type->is_nullable) {
        XrType *val_non_null = xr_type_non_nullable(l->isolate, val->type);
        if (val_non_null && xr_type_equals(target_type, val_non_null))
            return xi_lower_apply_primitive_type_view(l, node, val, target_type);
    }

    XrType *check_type = target_type;
    bool allow_null = target_type->is_nullable ||
                      xr_type_intrinsically_includes_null(target_type) ||
                      XR_TYPE_IS_NULL(target_type);
    if (target_type->is_nullable)
        check_type = xr_type_non_nullable(l->isolate, target_type);
    if (!check_type || XR_TYPE_IS_UNKNOWN(check_type) || XR_TYPE_IS_UNION(check_type))
        return val;

    uint8_t tid = xr_type_to_tid(check_type);
    if (tid == XR_TID_NULL && !XR_TYPE_IS_NULL(check_type))
        return val;

    XiValue *check = xi_value_new(l->func, l->cur_block, XI_CHECKTYPE, target_type, 1);
    if (!check)
        return val;
    check->args[0] = val;
    check->aux_int = ((int64_t) tid << 1) | (allow_null ? 1 : 0);
    check->line = (uint32_t) node->line;
    return check;
}

static XiFunc *lower_resolve_static_callee_func_in_scope(XiFunc *scope, XiValue *callee) {
    while (callee && xi_copy_is_identity_alias(callee) && callee->nargs >= 1)
        callee = callee->args[0];
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (XiFunc *) callee->aux;
    if (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW && callee->aux)
        return (XiFunc *) callee->aux;
    if (callee->op == XI_GET_SHARED) {
        int64_t slot = callee->aux_int;
        for (XiFunc *fn = scope; fn; fn = fn->parent_func) {
            if (fn->shared_slot_funcs && slot >= 0 && slot < (int64_t) fn->shared_slot_func_count &&
                fn->shared_slot_funcs[slot])
                return fn->shared_slot_funcs[slot];
        }
    }
    return NULL;
}

static XiFunc *lower_resolve_static_callee_func(XiLower *l, XiValue *callee) {
    return lower_resolve_static_callee_func_in_scope(l ? l->func : NULL, callee);
}

static void lower_instantiate_call_view_evidence(XiValue *call, const XiFunc *static_callee,
                                                 const struct XrType *callee_type,
                                                 bool has_receiver) {
    if (!call || !XR_TYPE_IS_SLICE(call->type))
        return;

    XrViewReturnSourceKind origin = XR_VIEW_RETURN_NONE;
    int source_param = -1;
    bool complete = false;
    if (static_callee && static_callee->view_return_complete) {
        origin = (XrViewReturnSourceKind) static_callee->view_return_source;
        source_param = static_callee->view_return_param;
        complete = true;
    } else if (callee_type && callee_type->kind == XR_KIND_FUNCTION) {
        origin = callee_type->function.view_return_source;
        source_param = callee_type->function.view_return_param;
        complete = callee_type->function.view_return_complete;
    }
    if (!complete)
        return;

    int source_operand = -1;
    if (origin == XR_VIEW_RETURN_PARAM)
        source_operand = source_param + 1; /* callee/receiver occupies args[0] */
    else if (origin == XR_VIEW_RETURN_RECEIVER && has_receiver)
        source_operand = 0;
    else if (origin != XR_VIEW_RETURN_STATIC)
        return;
    if (source_operand >= (int) call->nargs)
        return;

    call->view_evidence.origin = (uint8_t) origin;
    call->view_evidence.source_param = (int16_t) source_param;
    call->view_evidence.source_operand = (int16_t) source_operand;
    call->view_evidence.complete = 1;
    call->view_evidence.capability = 1; /* borrowed returns are readonly by default */
    call->view_evidence.lifetime = origin == XR_VIEW_RETURN_STATIC ? 2 : 1;
    if (source_operand >= 0 && call->args[source_operand])
        call->view_evidence.root_value_id = call->args[source_operand]->id;
    if (call->type->container.element_type)
        call->view_evidence.element_type_id = call->type->container.element_type->semantic_type_id;
}

/* Post-lowering rewrite: a direct call to a generator function does not run the
 * body — it constructs a coroutine-backed iterator. Rewrite XI_CALL -> XI_GEN_CALL
 * for every call whose static callee is a generator (entry_type == 2). This runs
 * after the whole function tree is lowered (so every callee's entry_type is set,
 * including forward/nested references) and before escape/ownership analysis (so
 * the generator call's coroutine-capture escape semantics are honored). The call
 * result type is already Iterator<T> (the generator's declared return type), so
 * only the opcode changes. */
static void xi_lower_rewrite_generator_calls_in(XiFunc *f) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL || v->nargs < 1)
                continue;
            XiFunc *callee = lower_resolve_static_callee_func_in_scope(f, v->args[0]);
            if (callee && callee->entry_type == 2 /* XR_ENTRY_GENERATOR */)
                v->op = XI_GEN_CALL;
        }
    }
    for (uint16_t c = 0; c < f->nchildren; c++)
        xi_lower_rewrite_generator_calls_in(f->children[c]);
}

XR_FUNC void xi_lower_rewrite_generator_calls(XiFunc *root) {
    xi_lower_rewrite_generator_calls_in(root);
}

static void lower_collect_read_place_params(XiLower *l, XiFunc *callee,
                                            const struct XrType *function_type,
                                            const XrParamMode *modes, int mode_count,
                                            bool *out_read_places, int out_count) {
    if (!out_read_places || out_count <= 0)
        return;
    memset(out_read_places, 0, (size_t) out_count * sizeof(*out_read_places));
    for (int i = 0; i < out_count; i++) {
        XrParamMode mode = modes && i < mode_count ? modes[i] : XR_PARAM_READ;
        if (mode != XR_PARAM_READ)
            continue;

        XiValue *param = callee && callee->params && i < callee->nparams ? callee->params[i] : NULL;
        bool read_place = xi_value_is_read_place_param(param);
        if (!read_place && function_type && function_type->kind == XR_KIND_FUNCTION &&
            i < function_type->function.param_count) {
            XrType *formal = xr_type_function_param_type(function_type, i);
            read_place = xi_lower_type_uses_read_place(l, formal);
        }
        out_read_places[i] = read_place;
        if (read_place && param)
            param->lowering_flags |= XI_LOWERING_FLAG_PARAM_READ_PLACE;
    }
}

static XrParamMode *lower_function_param_modes(XiLower *l, struct XrType *fn_type,
                                               XrParamMode *stack_modes, int stack_count,
                                               int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!fn_type || fn_type->kind != XR_KIND_FUNCTION || fn_type->function.param_count <= 0)
        return NULL;

    int count = fn_type->function.param_count;
    XrParamMode *modes = count <= stack_count
                             ? stack_modes
                             : (XrParamMode *) xi_func_arena_alloc(
                                   l->func, (uint32_t) ((size_t) count * sizeof(XrParamMode)));
    if (!modes)
        return NULL;
    for (int i = 0; i < count; i++)
        modes[i] = xr_type_function_param_mode(fn_type, i);
    if (out_count)
        *out_count = count;
    return modes;
}

static XiValue *xi_lower_narrow_for_native_field(XiLower *l, AstNode *node, XiValue *val,
                                                 uint8_t native_type);
static struct XrType *xi_lower_struct_field_type(XiLower *l, struct XrType *fallback,
                                                 XrAggregateLayout *layout, int field_index);

#define XI_LOWER_VALUE_LIST_STACK_CAP 32
#define XI_LOWER_MAX_VARIADIC_VALUES ((int) UINT16_MAX)

typedef struct XiLowerValueList {
    XiValue **items;
    int count;
    int cap;
} XiLowerValueList;

static void xi_lower_value_list_init(XiLowerValueList *list, XiValue **stack_items, int stack_cap) {
    list->items = stack_items;
    list->count = 0;
    list->cap = stack_cap;
}

static bool xi_lower_value_list_grow(XiLower *l, XiLowerValueList *list, int max_items) {
    int next_cap = list->cap > 0 ? list->cap * 2 : 1;
    if (next_cap <= list->count)
        next_cap = list->count + 1;
    if (next_cap > max_items)
        next_cap = max_items;
    if (next_cap <= list->cap)
        return false;

    XiValue **items = (XiValue **) xi_func_arena_alloc(
        l->func, (uint32_t) ((size_t) next_cap * sizeof(XiValue *)));
    if (!items) {
        l->had_error = true;
        return false;
    }
    if (list->count > 0)
        memcpy(items, list->items, (size_t) list->count * sizeof(XiValue *));
    list->items = items;
    list->cap = next_cap;
    return true;
}

static bool xi_lower_value_list_push(XiLower *l, XiLowerValueList *list, XiValue *value,
                                     int max_items, const char *what, int line) {
    if (list->count >= max_items) {
        fprintf(stderr, "[LOWER] %s exceeds %d at line %d\n", what ? what : "value count",
                max_items, line);
        l->had_error = true;
        return false;
    }
    if (list->count >= list->cap && !xi_lower_value_list_grow(l, list, max_items))
        return false;
    list->items[list->count++] = value;
    return true;
}

/* Propagate needs_cell along the transitive upvalue capture chain.
 * When an inner closure mutates a captured variable through SRC_UPVAL,
 * every intermediate level up to the defining SRC_REG capture needs
 * needs_cell=true so the emitter generates OP_CELL_NEW at the origin
 * and OP_CELL_GET/OP_CELL_SET at each forwarding level. */
static void propagate_needs_cell(XiLower *l, int upval_idx) {
    if (upval_idx < 0 || upval_idx >= (int) l->func->ncaptures)
        return;
    XiCapture *cap = &l->func->captures[upval_idx];
    cap->needs_cell = true;
    cap->is_mutable = true;
    cap->is_reassigned = true;

    /* Propagate upward through the transitive capture chain */
    if (cap->source == XI_CAPTURE_SRC_UPVAL && l->parent) {
        propagate_needs_cell(l->parent, (int) cap->index);
    } else if (cap->source == XI_CAPTURE_SRC_REG && l->parent && cap->name) {
        /* Mark the defining scope's variable so definitions survive DCE
         * and the emitter redirects writes through CELL_SET. */
        int parent_var = xi_lower_var_find(l->parent, 0, cap->name);
        if (parent_var >= 0 && parent_var < l->parent->var_count)
            l->parent->vars[parent_var].captured_by_child = true;
    }

    /* Propagate downward: child closures that already captured this
     * upvalue via SRC_UPVAL may have inherited needs_cell=false at
     * creation time.  Update them so the emitter generates CELL_GET. */
    for (uint16_t ci_fn = 0; ci_fn < l->func->nchildren; ci_fn++) {
        XiFunc *child = l->func->children[ci_fn];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
            if (child->captures[ci].source == XI_CAPTURE_SRC_UPVAL &&
                (int) child->captures[ci].index == upval_idx) {
                child->captures[ci].needs_cell = true;
                child->captures[ci].is_mutable = true;
                child->captures[ci].is_reassigned = true;
            }
        }
    }
}

/* ========== Expression Lowering ========== */

static XiValue *lower_literal(XiLower *l, AstNode *node) {
    struct XrType *analyzed_type = xa_analyzer_get_node_type(l->analyzer, node);
    XiValue *value = NULL;
    switch (node->type) {
        case AST_LITERAL_INT:
            /* xi_const_int and xi_const_float both pack into aux_int but with
             * different encodings (raw integer vs double bit pattern), and the
             * value's type is what tells every consumer which one it holds.  An
             * integer literal whose context is a float type is therefore a
             * *float* constant: emitting an integer payload under a float type
             * would reinterpret the bits and silently yield a denormal. */
            if (analyzed_type && XR_TYPE_IS_FLOAT(analyzed_type))
                value = xi_const_float(l->func, l->cur_block,
                                       (double) node->as.literal.raw_value.int_val, analyzed_type);
            else
                value = xi_const_int(l->func, l->cur_block, node->as.literal.raw_value.int_val,
                                     analyzed_type && XR_TYPE_IS_INT(analyzed_type) ? analyzed_type
                                                                                    : l->type_int);
            break;
        case AST_LITERAL_FLOAT:
            value = xi_const_float(
                l->func, l->cur_block, node->as.literal.raw_value.float_val,
                analyzed_type && XR_TYPE_IS_FLOAT(analyzed_type) ? analyzed_type : l->type_float);
            break;
        case AST_LITERAL_TRUE:
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);
        case AST_LITERAL_FALSE:
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        case AST_LITERAL_RUNE:
            return xi_const_rune(l->func, l->cur_block, node->as.literal.raw_value.rune_val,
                                 l->type_rune);
        case AST_LITERAL_NULL:
            return xi_const_null(l->func, l->cur_block, l->type_null);
        case AST_LITERAL_STRING:
            return xi_const_str(l->func, l->cur_block, node->as.literal.raw_value.string_val,
                                l->type_string);
        default:
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    XrConversionWitness conversion = {0};
    if (value && xa_typed_program_conversion(l->typed_program, node, &conversion))
        value->conversion = conversion;
    return value;
}

static XiValue *lower_ct_scalar_value(XiLower *l, const XrCtValue *value) {
    if (!l || !value)
        return NULL;
    switch (value->kind) {
        case XR_CT_INT:
            return xi_const_int(l->func, l->cur_block, value->as.int_val, l->type_int);
        case XR_CT_FLOAT:
            return xi_const_float(l->func, l->cur_block, value->as.float_val, l->type_float);
        case XR_CT_BOOL:
            return xi_const_bool(l->func, l->cur_block, value->as.bool_val, l->type_bool);
        case XR_CT_STRING:
            return xi_const_str(l->func, l->cur_block, value->as.string_val, l->type_string);
        case XR_CT_CHAR:
            return xi_const_rune(l->func, l->cur_block, value->as.rune_val, l->type_rune);
        case XR_CT_NULL:
            return xi_const_null(l->func, l->cur_block, l->type_null);
        default:
            return NULL;
    }
}

XR_FUNC XiValue *xi_lower_apply_numeric_conversion_witness(XiLower *l, AstNode *source_node,
                                                           XiValue *value,
                                                           struct XrType *target_type) {
    /* Nullability is not a numeric boundary.  A narrowed `int?` reaching an
     * `int` target differs from it only in nullability, which the dedicated
     * narrowing step owns; demanding a numeric conversion witness for it would
     * reject a program the analyzer proved correct.  The guard has to be
     * symmetric: neither side may be nullable. */
    if (!l || !source_node || !value || !value->type || !target_type || target_type->is_nullable ||
        value->type->is_nullable || !XR_TYPE_IS_NUMERIC(value->type) ||
        !XR_TYPE_IS_NUMERIC(target_type))
        return value;

    XrConversionWitness witness = {0};
    bool same_type = xr_type_equals(target_type, value->type);
    /* Any conversion internal to the source expression (most notably an
     * explicit `as`) has already been lowered.  An equal boundary type adds no
     * second conversion; contextual literal evidence is attached by
     * lower_literal itself. */
    if (same_type)
        return value;
    bool has_witness = xa_typed_program_conversion(l->typed_program, source_node, &witness);
    if (!has_witness) {
        fprintf(stderr,
                "[LOWER] numeric boundary lacks analyzer conversion evidence at line %d "
                "(boundary %s->%s)\n",
                (int) source_node->line, xr_scalar_rep_canonical_name(value->type->scalar_rep),
                xr_scalar_rep_canonical_name(target_type->scalar_rep));
        l->had_error = true;
        return NULL;
    }
    /* The analyzer's witness is the truth source for what conversion happens at
     * this boundary; the callee's declared parameter type is only a surface
     * shape.  Where they disagree it is because the analyzer resolved an
     * effective domain that the surface signature does not spell -- the public
     * math registry declares `(...args: float): float` while an all-int
     * min/max/clamp/abs call stays in the integer domain.  A witness whose
     * source and target reps are equal states that no conversion occurs, so
     * honour it and leave the value alone instead of manufacturing a boundary
     * the typed program never recorded. */
    if (witness.source_scalar_rep == value->type->scalar_rep &&
        witness.source_scalar_rep == witness.target_scalar_rep)
        return value;

    /* Name the clause that rejected the witness.  A bare "invalid evidence"
     * message cannot distinguish a wrong conversion kind from reps recorded
     * against a different type than the one that reached this boundary, and
     * those have completely different fixes. */
    const char *invalid_reason = NULL;
    if (!xr_conversion_kind_is_numeric(witness.kind) || witness.kind == XR_CONVERSION_DISALLOWED)
        invalid_reason = "kind is not an admissible numeric conversion";
    else if (witness.source_scalar_rep != value->type->scalar_rep)
        invalid_reason = "witness source rep does not match the value reaching the boundary";
    else if (witness.target_scalar_rep != target_type->scalar_rep)
        invalid_reason = "witness target rep does not match the boundary type";
    else if (witness.is_implicit && !xr_conversion_kind_is_implicit(witness.kind))
        invalid_reason = "kind is not implicit but the witness is marked implicit";
    if (invalid_reason) {
        fprintf(stderr,
                "[LOWER] numeric boundary has invalid conversion evidence '%s' at line %d: %s "
                "(witness %s->%s, boundary %s->%s)\n",
                xr_conversion_kind_name(witness.kind), (int) source_node->line, invalid_reason,
                xr_scalar_rep_canonical_name(witness.source_scalar_rep),
                xr_scalar_rep_canonical_name(witness.target_scalar_rep),
                xr_scalar_rep_canonical_name(value->type->scalar_rep),
                xr_scalar_rep_canonical_name(target_type->scalar_rep));
        l->had_error = true;
        return NULL;
    }
    if (!witness.is_implicit) {
        fprintf(stderr,
                "[LOWER] explicit numeric conversion evidence reached an implicit boundary at "
                "line %d\n",
                (int) source_node->line);
        l->had_error = true;
        return NULL;
    }

    /* The only remaining implicit numeric conversion is value-preserving
     * widening.  It has no runtime arithmetic, but a typed COPY keeps the
     * analyzer decision visible to Xi verification and backend dumps. */
    if (witness.kind != XR_CONVERSION_LOSSLESS_WIDEN) {
        fprintf(stderr,
                "[LOWER] implicit numeric boundary attempted non-lossless conversion '%s' at "
                "line %d\n",
                xr_conversion_kind_name(witness.kind), (int) source_node->line);
        l->had_error = true;
        return NULL;
    }
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, target_type, 1);
    if (!copy)
        return NULL;
    copy->args[0] = value;
    copy->conversion = witness;
    copy->line = (uint32_t) source_node->line;
    return copy;
}

static uint16_t xi_narrow_op_for_native_type(uint8_t native_type);

static bool xi_binary_needs_wrap(uint16_t op) {
    switch (op) {
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
            return true;
        default:
            return false;
    }
}

static XiValue *xi_lower_wrap_if_needed(XiLower *l, AstNode *node, XiValue *value,
                                        struct XrType *result_type, uint16_t source_op) {
    if (!value || !result_type || result_type->kind != XR_KIND_INT ||
        !xi_binary_needs_wrap(source_op))
        return value;
    uint16_t narrow_op = xi_narrow_op_for_native_type(result_type->scalar_rep);
    if (!narrow_op)
        return value;
    XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, result_type, 1);
    if (!n)
        return value;
    n->args[0] = value;
    n->line = (uint32_t) node->line;
    return n;
}

/* float32 precision boundaries: AOT narrows each operand to its static C type
 * at use (f32->float, f64->double, int->int64) and computes via C's usual
 * arithmetic conversions. The helpers below mirror that in the shared IR so the
 * VM and AOT stay bit-identical on mixed-precision arithmetic. */

/* Narrow a float32 operand to single precision before a wider (float64) op,
 * mirroring AOT's `(float)operand` at use. */
static XiValue *xi_lower_narrow_f32_operand(XiLower *l, AstNode *node, XiValue *v) {
    if (!v || !v->type || v->type->kind != XR_KIND_FLOAT || v->type->scalar_rep != XR_NATIVE_F32)
        return v;
    XiValue *n = xi_value_new(l->func, l->cur_block, XI_NARROW_F32, v->type, 1);
    if (!n)
        return v;
    n->args[0] = v;
    n->line = (uint32_t) node->line;
    return n;
}

static bool lower_str_concat_part(XiLower *l, AstNode *node, XiLowerValueList *parts, int line) {
    if (!node)
        return true;
    if (node->type == AST_BINARY_ADD) {
        /* Check if this ADD node has string result type */
        struct XrType *t = xa_analyzer_get_node_type(l->analyzer, node);
        if (t && t->kind == XR_KIND_STRING) {
            return lower_str_concat_part(l, node->as.binary.left, parts, line) &&
                   lower_str_concat_part(l, node->as.binary.right, parts, line);
        }
    }

    XiValue *part = xi_lower_expr(l, node);
    if (!part)
        return false;
    return xi_lower_value_list_push(l, parts, part, XI_LOWER_MAX_VARIADIC_VALUES,
                                    "string concat part count", line);
}

static XiValue *lower_binary(XiLower *l, AstNode *node) {
    /* Most &&/|| nodes are canonicalized to ternary to preserve short-circuit
     * semantics. Speculation-safe bool chains may intentionally remain here and
     * lower to XI_BAND/XI_BOR to avoid hot-path CFG/phi expansion. */

    /* String concat optimization: flatten ADD chain → XI_STR_CONCAT
     * which emits STRBUF_NEW/APPEND/FINISH (no intermediate allocs). */
    if (node->type == AST_BINARY_ADD) {
        struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
        if (result_type && result_type->kind == XR_KIND_STRING) {
            XiValue *stack_parts[XI_LOWER_VALUE_LIST_STACK_CAP];
            XiLowerValueList parts;
            xi_lower_value_list_init(&parts, stack_parts, XI_LOWER_VALUE_LIST_STACK_CAP);
            if (!lower_str_concat_part(l, node, &parts, node->line))
                return NULL;
            if (parts.count >= 2) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_STR_CONCAT, l->type_string,
                                          (uint16_t) parts.count);
                if (!v)
                    return NULL;
                for (int i = 0; i < parts.count; i++)
                    v->args[i] = parts.items[i];
                v->line = (uint32_t) node->line;
                return v;
            }
            if (parts.count == 1)
                return parts.items[0];
        }
    }

    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    if (!lhs || !rhs)
        return NULL;

    /* Prefer analyzer side table; fall back to local inference from operands */
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type) {
        result_type = xi_lower_infer_binary_type(l, node->type, lhs->type, rhs->type);
    }
    uint16_t op = xi_lower_binary_ast_to_xi_op(node->type);

    // float32 precision boundaries: keep the VM and AOT bit-identical on mixed
    // operands by mirroring AOT's per-operand narrowing in the shared IR.
    if (op == XI_ADD || op == XI_SUB || op == XI_MUL || op == XI_DIV) {
        if (result_type && result_type->kind == XR_KIND_FLOAT &&
            result_type->scalar_rep != XR_NATIVE_F32) {
            // float64 result with a float32 operand: narrow the float32 side to
            // single precision before the double op.
            lhs = xi_lower_narrow_f32_operand(l, node, lhs);
            rhs = xi_lower_narrow_f32_operand(l, node, rhs);
        }
    } else if (op == XI_EQ || op == XI_NE || op == XI_LT || op == XI_LE || op == XI_GT ||
               op == XI_GE) {
        // Comparisons: AOT narrows each f32 operand to float at use, so mirror
        // that for bit-identical results (e.g. (float)a == 0.1 is false even
        // though the stored double equals 0.1).
        lhs = xi_lower_narrow_f32_operand(l, node, lhs);
        rhs = xi_lower_narrow_f32_operand(l, node, rhs);
    }

    XiValue *raw = xi_binary(l->func, l->cur_block, op, result_type, lhs, rhs);
    return xi_lower_wrap_if_needed(l, node, raw, result_type, op);
}

static XiValue *lower_unary(XiLower *l, AstNode *node) {
    XiValue *operand = xi_lower_expr(l, node->as.unary.operand);
    if (!operand)
        return NULL;

    /* Prefer analyzer side table; fall back to local inference */
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type) {
        result_type = xi_lower_infer_unary_type(l, node->type, operand->type);
    }
    uint16_t op;

    switch (node->type) {
        case AST_UNARY_NEG:
            op = XI_NEG;
            break;
        case AST_UNARY_NOT:
            op = XI_NOT;
            break;
        case AST_UNARY_BNOT:
            op = XI_BNOT;
            break;
        default:
            op = XI_NEG;
            break;
    }

    XiValue *raw = xi_unary(l->func, l->cur_block, op, result_type, operand);
    if (op == XI_BNOT)
        return xi_lower_wrap_if_needed(l, node, raw, result_type, op);
    return raw;
}

static XiValue *lower_variable(XiLower *l, AstNode *node) {
    const char *name = node->as.variable.name;
    uint32_t sid = node->as.variable.symbol_id;
    if (xi_lower_symbol_is_sync_runtime_class(l, sid, name))
        return xi_lower_emit_builtin_class(l, name, node->line);

    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        /* Program-level top-level variables must be read from the
         * backing store because called functions can modify them,
         * which bypasses the local SSA and leaves it stale. */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            return xi_lower_emit_top_load(l, b, NULL);
        }
        if (l->vars[var_id].call_place) {
            XiValue *load =
                xi_value_new(l->func, l->cur_block, XI_PLACE_LOAD, l->vars[var_id].type, 1);
            if (!load)
                return NULL;
            load->args[0] = l->vars[var_id].call_place;
            load->var_id = (XiVarId) var_id;
            load->line = (uint32_t) node->line;
            return load;
        }
        XiValue *cur = xi_lower_braun_read(l, var_id, l->cur_block);
        if (cur && var_id < l->var_count && l->vars[var_id].captured_by_child) {
            XiValue *load = xi_value_new(l->func, l->cur_block, XI_COPY, cur->type, 1);
            if (load) {
                load->args[0] = cur;
                load->aux_int = XI_COPY_KIND_CELL_READ;
                load->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM;
                load->line = (uint32_t) node->line;
                return load;
            }
        }
        return cur;
    }

    /* Check for program-level variable from a nested scope */
    XiTopBinding tb = xi_lower_find_top_binding(l, sid, name);
    if (xi_top_binding_valid(tb)) {
        return xi_lower_emit_top_load(l, tb, NULL);
    }

    /* Not found locally — try upvalue capture from enclosing scope */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type)
            upval_type = l->type_any;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, upval_type, 0);
        if (v)
            v->aux_int = upval_idx;
        return v;
    }

    /* Builtin class names (PascalCase) resolve to runtime class globals.
     * Used as namespaces for static method dispatch like Json.parse(s). */
    if (name) {
        XiValue *module_func = xi_lower_builtin_module_function_ref(l, sid, name, (int) node->line);
        if (module_func)
            return module_func;

        XiValue *builtin_class = xi_lower_emit_builtin_class(l, name, node->line);
        if (builtin_class)
            return builtin_class;

        /* Builtin instance / value globals (camelCase / dunder) are populated
         * per script by xray_vm_set_script_info: `process` is the Process
         * instance carrying argv/cwd/argv0, `__file__` / `__dir__` are the
         * current module's source path and directory. */
        static const struct {
            const char *name;
            int index;
        } builtin_vars[] = {
            {"process", XR_GLOBAL_VAR_PROCESS},
            {"__file__", XR_GLOBAL_VAR_FILE},
            {"__dir__", XR_GLOBAL_VAR_DIR},
        };
        for (int i = 0; i < (int) (sizeof(builtin_vars) / sizeof(builtin_vars[0])); i++) {
            if (strcmp(name, builtin_vars[i].name) == 0) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, l->type_any, 0);
                if (v) {
                    v->aux_int = builtin_vars[i].index;
                    v->aux = (void *) builtin_vars[i].name;
                    v->line = (uint32_t) node->line;
                }
                return v;
            }
        }
    }

    /* Unresolved variable is a compiler bug: the analyzer must resolve
     * all variable references before lowering.  Hard-fail so the bug
     * surfaces immediately instead of hiding behind a runtime null. */
    fprintf(stderr, "[LOWER] unresolved variable '%s' (symbol_id=%u) at line %d\n",
            name ? name : "<null>", sid, (int) node->line);
    l->had_error = true;
    return NULL;
}

static void lower_assignment_mark_child_capture(XiLower *l, int var_id, const char *name,
                                                XiValue *val) {
    for (uint16_t ci_fn = 0; ci_fn < l->func->nchildren; ci_fn++) {
        XiFunc *child = l->func->children[ci_fn];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
            if (child->captures[ci].source == XI_CAPTURE_SRC_REG && child->captures[ci].name &&
                name && strcmp(child->captures[ci].name, name) == 0) {
                child->captures[ci].needs_cell = true;
                child->captures[ci].is_mutable = true;
                child->captures[ci].is_reassigned = true;
                if (var_id < l->var_count)
                    l->vars[var_id].captured_by_child = true;
                val->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
    }
}

static XiValue *lower_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.assignment.name;
    uint32_t sid = node->as.assignment.symbol_id;
    XiValue *val = xi_lower_expr(l, node->as.assignment.value);
    if (!val)
        return NULL;

    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        struct XrType *var_type = l->vars[var_id].type;
        val =
            xi_lower_apply_numeric_conversion_witness(l, node->as.assignment.value, val, var_type);
        if (!val)
            return NULL;
        val = xi_lower_apply_primitive_type_view(l, node, val, var_type);
        /* When assigning from a different variable (e.g. x = i), insert
         * an explicit copy so the target gets its own SSA value.  Without
         * this, braun_write stores the source variable's value directly,
         * and the shared SSA value causes two variables to coalesce to
         * the same physical register — corrupting loop-carried values
         * when the source variable is subsequently modified. */
        bool need_copy = (xi_var_id_is_valid(val->var_id) && val->var_id != (XiVarId) var_id);
        /* Value types (structs and fixed arrays) need independent storage on assignment. */
        bool value_clone_copy =
            xi_lower_value_needs_value_clone(l, val) && !xi_lower_value_is_fresh_value_struct(val);
        if (!need_copy && value_clone_copy)
            need_copy = true;
        if (need_copy) {
            XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, val->type, 1);
            if (copy) {
                copy->args[0] = val;
                if (value_clone_copy)
                    xi_lower_mark_value_clone_copy(copy);
                val = copy;
            }
        }
        if (l->vars[var_id].call_place) {
            XiValue *store = xi_value_new(l->func, l->cur_block, XI_PLACE_STORE, l->type_unit, 2);
            if (!store)
                return NULL;
            store->args[0] = l->vars[var_id].call_place;
            store->args[1] = val;
            store->line = (uint32_t) node->line;
        }
        xi_lower_braun_write(l, var_id, l->cur_block, val);

        /* If a child closure already captured this variable, retroactively
         * enable cell indirection so the closure sees the updated value.
         * Also mark captured_by_child so the new SSA value survives DCE
         * (the emitter redirects it through CELL_SET at emit time). */
        lower_assignment_mark_child_capture(l, var_id, name, val);

        /* If this is a program-level variable, also update backing store */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, val);
        }
        return val;
    }

    /* Check for program-level variable from nested scope */
    XiTopBinding tb = xi_lower_find_top_binding(l, sid, name);
    if (xi_top_binding_valid(tb)) {
        val = xi_lower_apply_numeric_conversion_witness(l, node->as.assignment.value, val, tb.type);
        if (!val)
            return NULL;
        val = xi_lower_apply_primitive_type_view(l, node, val, tb.type);
        xi_lower_emit_top_store(l, tb, val);
        return val;
    }

    /* Try upvalue store for captured mutable variable */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (upval_type && upval_type->kind == XR_KIND_INT &&
            upval_type->scalar_rep != XR_NATIVE_I64 && val->type && XR_TYPE_IS_INT(val->type)) {
            uint16_t narrow_op = xi_narrow_op_for_native_type(upval_type->scalar_rep);
            if (narrow_op) {
                XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, upval_type, 1);
                if (n) {
                    n->args[0] = val;
                    n->line = (uint32_t) node->line;
                    val = n;
                }
            }
        }
        val = xi_lower_apply_primitive_type_view(l, node, val, upval_type);
        /* Mark the capture as needing cell indirection because the child
         * mutates the captured variable.  The emit stage uses this to
         * emit CELL_NEW in the parent and CELL_GET/CELL_SET in the child. */
        XR_DCHECK(upval_idx < (int) l->func->ncaptures, "upval_idx out of range for needs_cell");
        propagate_needs_cell(l, upval_idx);

        XiValue *store = xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL, l->type_unit, 1);
        if (store) {
            store->args[0] = val;
            store->aux_int = upval_idx;
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
        return val;
    }

    /* Unresolved assignment target is a compiler bug: the analyzer must
     * bind all assignment targets before lowering. */
    fprintf(stderr, "[LOWER] unresolved assignment target '%s' (symbol_id=%u) at line %d\n",
            name ? name : "<null>", sid, (int) node->line);
    l->had_error = true;
    return NULL;
}

static int json_field_index(struct XrType *type, const char *name) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !type->object.field_names || !name)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (!type->object.field_names[i])
            return -1;
    }
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static const char *lower_static_string_key(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    if (!node || node->type != AST_LITERAL_STRING)
        return NULL;
    return node->as.literal.raw_value.string_val;
}

static const XiImportRef *lower_import_ref_from_value(XiLower *l, const XiValue *v) {
    while (v &&
           (v->op == XI_COPY || xi_op_is_identity_forward(v->op) || v->op == XI_BOX ||
            v->op == XI_UNBOX || v->op == XI_RETAIN || v->op == XI_RELEASE) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    if (!v)
        return NULL;
    if (v->op == XI_IMPORT_REF && v->aux)
        return (const XiImportRef *) v->aux;
    if (v->op != XI_GET_SHARED || v->aux_int < 0)
        return NULL;
    int slot = (int) v->aux_int;
    for (XiLower *p = l; p; p = p->parent) {
        if (p->is_program && slot < p->var_cap)
            return p->shared_slot_imports ? p->shared_slot_imports[slot] : NULL;
    }
    return NULL;
}

static bool lower_value_is_whole_module_import(XiLower *l, const XiValue *v,
                                               const char *module_name) {
    const XiImportRef *ref = lower_import_ref_from_value(l, v);
    return ref && ref->module_path && strcmp(ref->module_path, module_name) == 0 &&
           ref->member_name == NULL;
}

static bool lower_call_object_is_module(XiLower *l, AstNode *object, const char *module_name) {
    if (!l || !l->analyzer || !object || object->type != AST_VARIABLE || !module_name)
        return false;

    VariableNode *var = &object->as.variable;
    XaSymbol *sym = NULL;
    if (var->symbol_id)
        sym = xa_scope_lookup_by_id(l->analyzer->global_scope, var->symbol_id);
    if (!sym && var->name)
        sym = xa_analyzer_lookup(l->analyzer, var->name);
    if (!sym && var->name)
        sym = xa_analyzer_lookup_in_scope(l->analyzer, var->name, l->analyzer->global_scope);
    if (!sym && var->name)
        sym = xa_analyzer_lookup_deep(l->analyzer, var->name);
    if (!sym || sym->kind != XA_SYM_MODULE)
        return false;

    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return links && links->module_name && strcmp(links->module_name, module_name) == 0;
}

static const char *lower_call_callee_imported_member(XiLower *l, AstNode *callee,
                                                     const char *module_name) {
    if (!l || !l->analyzer || !callee || callee->type != AST_VARIABLE || !module_name)
        return NULL;

    VariableNode *var = &callee->as.variable;
    XaSymbol *sym = NULL;
    if (var->symbol_id)
        sym = xa_scope_lookup_by_id(l->analyzer->global_scope, var->symbol_id);
    if (!sym && var->name)
        sym = xa_analyzer_lookup(l->analyzer, var->name);
    if (!sym && var->name)
        sym = xa_analyzer_lookup_in_scope(l->analyzer, var->name, l->analyzer->global_scope);
    if (!sym && var->name)
        sym = xa_analyzer_lookup_deep(l->analyzer, var->name);
    if (!sym)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    if (!links || !links->module_name || strcmp(links->module_name, module_name) != 0)
        return NULL;

    return links->import_member_name ? links->import_member_name
                                     : (sym->name ? sym->name : var->name);
}

static const XaParallelCallPlan *lower_analyzer_parallel_call_plan(XiLower *l, AstNode *call_node) {
    if (!l || !l->analyzer || !call_node)
        return NULL;
    return xa_analyzer_get_parallel_call_plan(l->analyzer, call_node);
}

static XiValue *lower_emit_field_load(XiLower *l, XiValue *obj, const char *name,
                                      struct XrType *result_type, int line) {
    if (!l || !obj || !name)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->aux = (void *) arena_strdup(l->func, name);
    v->aux_int = xi_lower_method_symbol(l, name);
    v->flags |= XI_FLAG_READS_MEM;
    v->line = (uint32_t) line;
    xi_lower_bind_class_field_id(l, v, obj->type, name);
    return v;
}

static XiValue *lower_emit_len(XiLower *l, XiValue *value, int line) {
    if (!l || !value)
        return NULL;
    XiValue *len = xi_value_new(l->func, l->cur_block, XI_LEN, l->type_int, 1);
    if (!len)
        return NULL;
    len->args[0] = value;
    len->line = (uint32_t) line;
    return len;
}

static XiValue *lower_bind_parallel_intrinsic_result(XiLower *l, uint32_t first_value_id,
                                                     XaIntrinsicId intrinsic_id, XiValue *result) {
    if (!l || !l->func || !result || intrinsic_id == XA_INTRINSIC_NONE)
        return result;
    XiValue *semantic = NULL;
    for (uint32_t bi = 0; bi < l->func->nblocks; bi++) {
        XiBlock *block = l->func->blocks[bi];
        for (uint32_t vi = 0; block && vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || value->id < first_value_id ||
                (value->op != XI_PAR_FOR && value->op != XI_PAR_MAP && value->op != XI_PAR_REDUCE))
                continue;
            if (semantic) {
                l->had_error = true;
                fprintf(stderr,
                        "error: canonical parallel call emitted more than one semantic Xi op\n");
                return NULL;
            }
            semantic = value;
        }
    }
    if (!semantic) {
        l->had_error = true;
        fprintf(stderr, "error: canonical parallel call emitted no semantic Xi op\n");
        return NULL;
    }
    semantic->xa_intrinsic_id = intrinsic_id;
    return result;
}

static XiValue *lower_parallel_module_intrinsic_or_error(XiLower *l, AstNode *node,
                                                         CallExprNode *call,
                                                         XaParallelCallKind kind,
                                                         XaIntrinsicId intrinsic_id) {
    uint32_t first_value_id = l && l->func ? l->func->next_value_id : 0;
    XiValue *parallel_intrinsic = lower_parallel_module_intrinsic_call(l, node, call, kind);
    if (parallel_intrinsic)
        return lower_bind_parallel_intrinsic_result(l, first_value_id, intrinsic_id,
                                                    parallel_intrinsic);
    if ((l && l->had_error) || kind == XA_PAR_CALL_NONE)
        return NULL;

    if (l)
        l->had_error = true;
    if (kind == XA_PAR_CALL_MAP_INTO) {
        fprintf(stderr,
                "error: parallel.mapInto expected (Range, output, inline (item) lambda[, "
                "literal parallel.Options(...)]) at line %d\n",
                node ? node->line : -1);
    } else if (kind == XA_PAR_CALL_REDUCE) {
        fprintf(stderr,
                "error: parallel.reduce expected (Range, initial, inline (item) body, inline "
                "combine lambda[, literal parallel.Options(...)]) at line %d\n",
                node ? node->line : -1);
    } else {
        const char *source_name = xa_parallel_call_kind_name(kind);
        fprintf(stderr,
                "error: parallel.%s expected (Range, inline (item) lambda[, literal "
                "parallel.Options(...)]) at line %d\n",
                source_name ? source_name : "<invalid>", node ? node->line : -1);
    }
    return NULL;
}

static bool lower_mem_layout_member_name(const char *name) {
    return name && (strcmp(name, "sizeOf") == 0 || strcmp(name, "alignOf") == 0 ||
                    strcmp(name, "offsetOf") == 0);
}

static bool lower_type_is_target_width_int(const XrType *type, uint8_t *out_native) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    if (type->scalar_rep != XR_NATIVE_ISIZE && type->scalar_rep != XR_NATIVE_USIZE)
        return false;
    if (out_native)
        *out_native = type->scalar_rep;
    return true;
}

static XiValue *lower_mem_layout_call(XiLower *l, AstNode *node, CallExprNode *call,
                                      const char *member) {
    if (!l || !node || !call || !lower_mem_layout_member_name(member) ||
        call->type_arg_count != 1 || !call->type_args || !call->type_args[0])
        return NULL;

    XrType *target = l->analyzer ? xr_tref_resolve_in_analyzer(l->analyzer, call->type_args[0])
                                 : xr_tref_resolve(l->isolate, call->type_args[0]);
    target = xi_lower_type_or_any(l, target, "mem layout type argument", node->line);
    uint32_t size = 0;
    uint32_t align = 0;
    if (!xr_type_has_static_layout(xi_lower_target_data_layout(l), target, &size, &align))
        return NULL;

    uint32_t value = 0;
    if (strcmp(member, "sizeOf") == 0) {
        if (call->arg_count != 0)
            return NULL;
        uint8_t native = 0;
        if (lower_type_is_target_width_int(target, &native)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_TARGET_SIZEOF, l->type_int, 0);
            if (v)
                v->aux_int = native;
            return v;
        }
        value = size;
    } else if (strcmp(member, "alignOf") == 0) {
        if (call->arg_count != 0)
            return NULL;
        uint8_t native = 0;
        if (lower_type_is_target_width_int(target, &native)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_TARGET_ALIGNOF, l->type_int, 0);
            if (v)
                v->aux_int = native;
            return v;
        }
        value = align;
    } else {
        if (call->arg_count != 1 || !call->arguments || !call->arguments[0] ||
            call->arguments[0]->type != AST_LITERAL_STRING ||
            !call->arguments[0]->as.literal.raw_value.string_val)
            return NULL;
        if (!xr_type_has_static_field_offset(xi_lower_target_data_layout(l), target,
                                             call->arguments[0]->as.literal.raw_value.string_val,
                                             &value))
            return NULL;
    }

    return xi_const_int(l->func, l->cur_block, (int64_t) value, l->type_int);
}

static XiValue *lower_mem_addr_pointer_call(XiLower *l, AstNode *node, CallExprNode *call,
                                            const char *member) {
    if (!l || !node || !call || !member || strcmp(member, "addr") != 0 || call->arg_count != 1 ||
        call->type_arg_count != 0 || !call->arguments || !call->arguments[0])
        return NULL;

    struct XrType *arg_type = xi_lower_node_type(l, call->arguments[0]);
    if (!arg_type || !XR_TYPE_IS_POINTER(arg_type))
        return NULL;

    XiValue *ptr = xi_lower_expr(l, call->arguments[0]);
    if (!ptr)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (!result_type || !XR_TYPE_IS_INT(result_type))
        result_type = l->type_int;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CONVERT, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = ptr;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_mem_pointer_constructor_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                   const char *member) {
    if (!l || !node || !call || !member ||
        (strcmp(member, "ptr") != 0 && strcmp(member, "mutPtr") != 0) || call->arg_count != 1 ||
        call->type_arg_count != 1 || !call->arguments || !call->arguments[0])
        return NULL;

    XiValue *addr = xi_lower_expr(l, call->arguments[0]);
    struct XrType *result_type = xi_lower_node_type(l, node);
    if (!addr || !result_type || !XR_TYPE_IS_POINTER(result_type))
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CONVERT, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = addr;
    v->line = (uint32_t) node->line;
    return v;
}

static int64_t xi_pack_slice_from_ptr_aux(XiLower *l, struct XrType *type);

static XiValue *lower_mem_slice_call(XiLower *l, AstNode *node, CallExprNode *call,
                                     const char *member) {
    if (!l || !node || !call || !member || strcmp(member, "slice") != 0 || call->arg_count != 3 ||
        call->type_arg_count > 1 || !call->arguments || !call->arguments[0] ||
        !call->arguments[1] || !call->arguments[2])
        return NULL;
    XiValue *ptr = xi_lower_expr(l, call->arguments[0]);
    XiValue *count = xi_lower_expr(l, call->arguments[1]);
    XiValue *owner = xi_lower_expr(l, call->arguments[2]);
    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *elem_type =
        result_type && XR_TYPE_IS_SLICE(result_type) ? result_type->container.element_type : NULL;
    int64_t layout_aux = xi_pack_slice_from_ptr_aux(l, elem_type);
    if (!ptr || !count || !owner || !ptr->type || !XR_TYPE_IS_POINTER(ptr->type) || !result_type ||
        !XR_TYPE_IS_SLICE(result_type) || layout_aux == 0)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_SLICE_FROM_PTR, result_type, 3);
    if (!v)
        return NULL;
    v->args[0] = ptr;
    v->args[1] = count;
    v->args[2] = owner;
    v->aux_int = layout_aux;
    v->aux = xi_lower_type_struct_layout(l, elem_type);
    /* mem.slice is already an unsafe boundary. The caller proves non-negative
     * count, non-null/aligned storage for a non-empty view, and address-range
     * validity; owner remains explicit lifetime evidence. */
    v->flags = XI_FLAG_READS_MEM;
    v->line = (uint32_t) node->line;
    v->view_evidence.origin = XI_VIEW_ORIGIN_FOREIGN;
    v->view_evidence.source_operand = 2;
    v->view_evidence.source_param = -1;
    v->view_evidence.root_value_id = owner->id;
    v->view_evidence.element_type_id = elem_type ? elem_type->semantic_type_id : 0;
    v->view_evidence.capability = 1;
    v->view_evidence.lifetime = 1;
    v->view_evidence.complete = 1;
    return v;
}

static XiValue *lower_mem_assume_initialized_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                  const char *member) {
    if (!l || !node || !call || !member || strcmp(member, "assumeInitialized") != 0 ||
        call->arg_count != 1 || call->type_arg_count != 1 || !call->arguments ||
        !call->arguments[0])
        return NULL;
    XrType *target = xi_lower_node_type(l, node);
    if (!target || XR_TYPE_IS_UNKNOWN(target))
        return NULL;
    uint32_t size = 0;
    uint32_t align = 0;
    if (!xr_type_has_static_layout(xa_analyzer_target_data_layout(l->analyzer), target, &size,
                                   &align) ||
        size == 0 || align == 0 || align > UINT16_MAX)
        return NULL;
    XiValue *buffer = xi_lower_expr(l, call->arguments[0]);
    if (!buffer)
        return NULL;
    const XrAggregateLayout *layout = xi_lower_type_struct_layout(l, target);
    uint8_t code =
        layout ? XI_BUFFER_MATERIALIZE_AGGREGATE : xr_ffi_type_from_xrtype(target, false);
    if (!layout && !xr_ffi_type_is_memory_scalar(code))
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_BUFFER_MATERIALIZE, target, 1);
    if (!v)
        return NULL;
    v->args[0] = buffer;
    v->aux_int = XI_BUFFER_MATERIALIZE_AUX(code, size, align);
    v->aux = (void *) layout;
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_mem_with_slice_mut_call(XiLower *l, AstNode *node, CallExprNode *call,
                                              const char *member) {
    if (!l || !node || !call || !member || strcmp(member, "withSliceMut") != 0 ||
        call->arg_count != 4 || call->type_arg_count > 1 || !call->arguments ||
        !call->arguments[0] || !call->arguments[1] || !call->arguments[2] || !call->arguments[3])
        return NULL;

    XiValue *ptr = xi_lower_expr(l, call->arguments[0]);
    XiValue *count = xi_lower_expr(l, call->arguments[1]);
    XiValue *guard = xi_lower_expr(l, call->arguments[2]);
    XiValue *callback = xi_lower_expr(l, call->arguments[3]);
    if (!ptr || !count || !guard || !callback || !ptr->type || !XR_TYPE_IS_POINTER(ptr->type))
        return NULL;

    struct XrType *elem_type = call->type_arg_count == 1 && call->type_args && call->type_args[0]
                                   ? xr_tref_resolve_in_analyzer(l->analyzer, call->type_args[0])
                                   : ptr->type->container.element_type;
    struct XrType *slice_type = xr_type_new_slice(l->isolate, elem_type);
    int64_t layout_aux = xi_pack_slice_from_ptr_aux(l, elem_type);
    if (!slice_type || layout_aux == 0)
        return NULL;

    XiValue *slice = xi_value_new(l->func, l->cur_block, XI_SLICE_FROM_PTR, slice_type, 3);
    if (!slice)
        return NULL;
    slice->args[0] = ptr;
    slice->args[1] = count;
    slice->args[2] = guard;
    slice->aux_int = layout_aux | XI_SLICE_FROM_PTR_AUX_MUTABLE;
    slice->aux = xi_lower_type_struct_layout(l, elem_type);
    slice->flags = XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    slice->line = (uint32_t) node->line;
    slice->view_evidence.origin = XI_VIEW_ORIGIN_FOREIGN;
    slice->view_evidence.source_operand = 2;
    slice->view_evidence.source_param = -1;
    slice->view_evidence.root_value_id = guard->id;
    slice->view_evidence.element_type_id = elem_type ? elem_type->semantic_type_id : 0;
    slice->view_evidence.capability = 2;
    slice->view_evidence.lifetime = 1;
    slice->view_evidence.complete = 1;

    XiValue *place = xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR, slice_type, 1);
    if (!place)
        return NULL;
    place->args[0] = slice;
    place->line = (uint32_t) node->line;

    XiCallPlan *plan = (XiCallPlan *) xi_func_arena_alloc(l->func, sizeof(*plan));
    XiCallArgPlan *arg_plan = (XiCallArgPlan *) xi_func_arena_alloc(l->func, sizeof(*arg_plan));
    if (!plan || !arg_plan)
        return NULL;
    memset(plan, 0, sizeof(*plan));
    memset(arg_plan, 0, sizeof(*arg_plan));
    plan->args = arg_plan;
    plan->nargs = 1;
    plan->verified = true;
    arg_plan->param_mode = XR_PARAM_REF;
    arg_plan->access = XR_CALL_ARG_REF;
    arg_plan->origin = XI_PLACE_ORIGIN_PROJECTION_TEMP;
    arg_plan->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    arg_plan->escape = XI_PLACE_ESCAPE_NONE;
    arg_plan->addressable = true;
    arg_plan->origin_var_id = XI_NO_VAR_ID;
    arg_plan->place = place;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *invoke = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, 2);
    if (!invoke)
        return NULL;
    invoke->args[0] = callback;
    invoke->args[1] = place;
    invoke->call_plan = plan;
    invoke->flags |=
        XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    invoke->line = (uint32_t) node->line;
    xi_lower_bind_callsite_id(l, invoke, xi_lower_source_node_id(l, node));
    xi_lower_insert_err_check(l, node, true);
    return invoke;
}

static bool lower_math_constant(XiLower *l, const char *name, XiValue **out) {
    if (!out)
        return false;
    if (out)
        *out = NULL;
    if (!name)
        return false;

    if (strcmp(name, "PI") == 0)
        *out = xi_const_float(l->func, l->cur_block, 3.14159265358979323846, l->type_float);
    else if (strcmp(name, "E") == 0)
        *out = xi_const_float(l->func, l->cur_block, 2.71828182845904523536, l->type_float);
    else if (strcmp(name, "TAU") == 0)
        *out = xi_const_float(l->func, l->cur_block, 6.28318530717958647692, l->type_float);
    else if (strcmp(name, "SQRT2") == 0)
        *out = xi_const_float(l->func, l->cur_block, 1.41421356237309504880, l->type_float);
    else if (strcmp(name, "LN2") == 0)
        *out = xi_const_float(l->func, l->cur_block, 0.69314718055994530942, l->type_float);
    else if (strcmp(name, "LN10") == 0)
        *out = xi_const_float(l->func, l->cur_block, 2.30258509299404568402, l->type_float);
    else if (strcmp(name, "LOG2E") == 0)
        *out = xi_const_float(l->func, l->cur_block, 1.44269504088896340736, l->type_float);
    else if (strcmp(name, "LOG10E") == 0)
        *out = xi_const_float(l->func, l->cur_block, 0.43429448190325182765, l->type_float);
    else if (strcmp(name, "EPSILON") == 0)
        *out = xi_const_float(l->func, l->cur_block, DBL_EPSILON, l->type_float);
    else if (strcmp(name, "MAX_INT") == 0)
        *out = xi_const_int(l->func, l->cur_block, INT64_MAX, l->type_int);
    else if (strcmp(name, "MIN_INT") == 0)
        *out = xi_const_int(l->func, l->cur_block, INT64_MIN, l->type_int);
    else if (strcmp(name, "MAX_FLOAT") == 0)
        *out = xi_const_float(l->func, l->cur_block, DBL_MAX, l->type_float);
    else if (strcmp(name, "INF") == 0)
        *out = xi_const_float(l->func, l->cur_block, INFINITY, l->type_float);
    else if (strcmp(name, "NAN") == 0)
        *out = xi_const_float(l->func, l->cur_block, NAN, l->type_float);
    else
        return false;

    return *out != NULL;
}

static bool lower_encoding_constant(XiLower *l, const char *name, XiValue **out) {
    if (!out)
        return false;
    *out = NULL;
    if (!name)
        return false;

    if (strcmp(name, "LE") == 0)
        *out = xi_const_int(l->func, l->cur_block, XR_ENCODING_UTF16_LE, l->type_int);
    else if (strcmp(name, "BE") == 0)
        *out = xi_const_int(l->func, l->cur_block, XR_ENCODING_UTF16_BE, l->type_int);
    else
        return false;

    return *out != NULL;
}

/* Math registry member behind a plain `f(...)` callee, resolved from the same
 * analyzer symbol record the analyzer itself consulted when it typed the call.
 * Reading it here keeps lowering's effective-domain decision identical to the
 * analyzer's instead of re-deriving one from the Xi-side import binding, which
 * is not reachable for every callee shape. */
static const char *lower_math_member_from_callee_symbol(XiLower *l, CallExprNode *call) {
    if (!l || !l->analyzer || !call || !call->callee || call->callee->type != AST_VARIABLE)
        return NULL;
    const char *name = call->callee->as.variable.name;
    if (!name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup_deep(l->analyzer, name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    if (!links || !links->module_name || strcmp(links->module_name, "math") != 0)
        return NULL;
    return links->import_member_name ? links->import_member_name : name;
}

static bool lower_math_call_arity_ok(const char *name, int nargs) {
    if (!name || nargs < 0)
        return false;
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
        return true;
    if (strcmp(name, "pow") == 0 || strcmp(name, "atan2") == 0 || strcmp(name, "hypot") == 0 ||
        strcmp(name, "fmod") == 0)
        return nargs == 2;
    if (strcmp(name, "clamp") == 0 || strcmp(name, "lerp") == 0)
        return nargs == 3;
    static const char *unary[] = {
        "abs",  "floor", "ceil",  "round", "sqrt",     "sin",      "cos",  "tan",   "asin",
        "acos", "atan",  "log",   "log10", "log2",     "exp",      "sinh", "cosh",  "tanh",
        "cbrt", "trunc", "log1p", "expm1", "degToRad", "radToDeg", "sign", "isNaN", "isFinite",
    };
    for (int i = 0; i < (int) (sizeof(unary) / sizeof(unary[0])); i++) {
        if (strcmp(name, unary[i]) == 0)
            return nargs == 1;
    }
    return false;
}

static bool lower_math_args_all_int(XiValue **arg_vals, int arg_count) {
    if (!arg_vals || arg_count <= 0)
        return false;
    for (int i = 0; i < arg_count; i++) {
        if (!arg_vals[i] || !arg_vals[i]->type || !XR_TYPE_IS_INT(arg_vals[i]->type))
            return false;
    }
    return true;
}

static bool lower_math_call_preserves_int_args(const char *member, XiValue **arg_vals,
                                               int arg_count) {
    if (!member || !lower_math_args_all_int(arg_vals, arg_count))
        return false;
    return (strcmp(member, "abs") == 0 && arg_count == 1) || strcmp(member, "min") == 0 ||
           strcmp(member, "max") == 0 || (strcmp(member, "clamp") == 0 && arg_count == 3);
}

static struct XrType *lower_math_call_result_type(XiLower *l, const char *member,
                                                  XiValue **arg_vals, int arg_count) {
    if (strcmp(member, "abs") == 0 && arg_count == 1 &&
        lower_math_args_all_int(arg_vals, arg_count))
        return l->type_any;
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) && arg_count == 0)
        return l->type_any;
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) &&
        lower_math_args_all_int(arg_vals, arg_count))
        return l->type_int;
    if (strcmp(member, "clamp") == 0 && arg_count == 3 &&
        lower_math_args_all_int(arg_vals, arg_count))
        return l->type_int;
    if (strcmp(member, "floor") == 0 || strcmp(member, "ceil") == 0 ||
        strcmp(member, "round") == 0 || strcmp(member, "trunc") == 0 || strcmp(member, "sign") == 0)
        return l->type_int;
    if (strcmp(member, "isNaN") == 0 || strcmp(member, "isFinite") == 0)
        return l->type_bool;
    return l->type_float;
}

static bool lower_type_has_sequence_evidence(const struct XrType *type) {
    return type && (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_SLICE(type) || XR_TYPE_IS_STRING(type) ||
                    xr_type_is_builtin_named_class(type, "StringBuilder"));
}

static XiValue *lower_type_namespace_member(XiLower *l, const MemberAccessNode *ma) {
    if (!ma->object || ma->object->type != AST_VARIABLE || !ma->object->as.variable.name ||
        strcmp(ma->object->as.variable.name, "Type") != 0)
        return NULL;
    int tid = xi_lower_type_constant_id(ma->name);
    if (tid < 0)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_int, 0);
    if (v)
        v->aux_int = tid;
    return v;
}

static XiValue *lower_module_member_constant(XiLower *l, XiValue *obj, const char *name) {
    XiValue *constant = NULL;
    if (lower_value_is_whole_module_import(l, obj, "math") &&
        lower_math_constant(l, name, &constant))
        return constant;
    if (lower_value_is_whole_module_import(l, obj, "encoding") &&
        lower_encoding_constant(l, name, &constant))
        return constant;
    return NULL;
}

/* Payload descriptor type IDs are specialization-dependent: the declaration
 * metadata for `Result<T>.Ok(T)` contains T, while `Result<int>.variants`
 * must expose int.  Lower the finite lookup to scalar Xi selects so VM and
 * AOT share the same concrete answer without cloning or mutating the runtime
 * enum namespace for each specialization. */
static XiValue *lower_enum_payload_type_id(XiLower *l, XrType *owner, XaSymbol *enum_sym,
                                           XiValue *descriptor, uint32_t line) {
    if (!l || !owner || owner->kind != XR_KIND_ENUM || !enum_sym || !descriptor)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, enum_sym);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    if (!info || !info->variants)
        return NULL;

    int param_count = xa_symbol_links_get_type_param_count(links);
    const char *stack_names[8];
    const char **param_names = stack_names;
    if (param_count > 8) {
        param_names = (const char **) xr_malloc(sizeof(const char *) * (size_t) param_count);
        if (!param_names)
            return NULL;
    }
    for (int i = 0; i < param_count; i++)
        param_names[i] = xa_symbol_links_get_type_param_name(links, i);

    XiValue *result = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    for (int vi = (int) info->variant_count - 1; vi >= 0; vi--) {
        XaEnumVariantInfo *variant = &info->variants[vi];
        for (int pi = (int) variant->payload_count - 1; pi >= 0; pi--) {
            XrType *payload_type = variant->payload_types ? variant->payload_types[pi] : NULL;
            if (param_count > 0 && owner->enum_type.type_arg_count == param_count &&
                owner->enum_type.type_args) {
                payload_type = xr_type_substitute(l->isolate, payload_type, param_names,
                                                  owner->enum_type.type_args, param_count);
            }
            int64_t packed = (int64_t) (((uint64_t) (uint32_t) vi << 32) | (uint32_t) pi);
            XiValue *key = xi_const_int(l->func, l->cur_block, packed, l->type_int);
            XiValue *condition =
                xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, descriptor, key);
            XiValue *type_id = xi_const_int(l->func, l->cur_block,
                                            (int64_t) xr_type_to_tid(payload_type), l->type_int);
            XiValue *select = xi_value_new(l->func, l->cur_block, XI_SELECT, l->type_int, 3);
            if (!key || !condition || !type_id || !select) {
                if (param_names != stack_names)
                    xr_free((void *) param_names);
                return NULL;
            }
            select->args[0] = condition;
            select->args[1] = type_id;
            select->args[2] = result;
            select->line = line;
            result = select;
        }
    }
    if (param_names != stack_names)
        xr_free((void *) param_names);
    if (result) {
        result->enum_metadata_owner = owner;
        result->enum_metadata_field = XA_ENUM_META_PAYLOAD_TYPE;
        result->enum_metadata_kind = descriptor->enum_metadata_kind != XR_ENUM_METADATA_NONE
                                         ? descriptor->enum_metadata_kind
                                         : (uint8_t) xr_type_enum_metadata_kind(descriptor->type);
    }
    return result;
}

static XiValue *lower_mark_enum_metadata(XiValue *value, XrType *owner, uint8_t field) {
    if (value) {
        value->enum_metadata_owner = owner;
        value->enum_metadata_field = field;
        value->enum_metadata_kind = (uint8_t) xr_type_enum_metadata_kind(value->type);
    }
    return value;
}

static XiValue *lower_mark_enum_metadata_from(XiValue *value, XrType *owner, uint8_t field,
                                              const XiValue *descriptor) {
    value = lower_mark_enum_metadata(value, owner, field);
    if (value && descriptor) {
        uint8_t kind = descriptor->enum_metadata_kind != XR_ENUM_METADATA_NONE
                           ? descriptor->enum_metadata_kind
                           : (uint8_t) xr_type_enum_metadata_kind(descriptor->type);
        if (kind != XR_ENUM_METADATA_NONE)
            value->enum_metadata_kind = kind;
    }
    return value;
}

static bool lower_symbol_has_enum_schema(XiLower *l, XaSymbol *sym) {
    if (!l || !sym)
        return false;
    if (sym->kind == XA_SYM_ENUM)
        return true;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return sym->kind == XA_SYM_IMPORT && links && links->type &&
           links->type->kind == XR_KIND_ENUM && links->enum_info;
}

static bool lower_selected_enum_member_access(XiLower *l, AstNode *node, const XaSelection *sel,
                                              XiValue **out) {
    MemberAccessNode *ma = &node->as.member_access;
    *out = NULL;
    if (!sel)
        return false;

    if (sel->kind == XA_SEL_ENUM_VARIANTS && sel->result_type) {
        XrType *owner = xr_type_enum_metadata_owner(sel->result_type);
        int64_t count =
            owner && owner->enum_type.layout ? (int64_t) owner->enum_type.layout->variant_count : 0;
        XiValue *view = xi_value_new(l->func, l->cur_block, XI_CONST, sel->result_type, 0);
        if (view) {
            view->aux_int = count;
            view->line = (uint32_t) node->line;
            lower_mark_enum_metadata(view, owner, XA_ENUM_META_VARIANTS);
        }
        *out = view;
        return true;
    }

    if (sel->kind == XA_SEL_ENUM_MEMBER && sel->target_symbol &&
        sel->target_symbol->kind == XA_SYM_ENUM && ma->name) {
        const char *enum_name = sel->target_symbol->name;
        XiValue *enum_val =
            xi_lower_enum_namespace_value(l, sel->target_symbol, enum_name, (int) node->line);
        if (!enum_val)
            return false;
        struct XrType *result_type =
            sel->result_type ? sel->result_type : xi_lower_node_type(l, node);
        XiValue *value = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
        if (value) {
            value->args[0] = enum_val;
            value->aux = (void *) arena_strdup(l->func, ma->name);
            value->aux_int = xi_lower_method_symbol(l, ma->name);
            value->line = (uint32_t) node->line;
        }
        *out = value;
        return true;
    }

    if (sel->kind != XA_SEL_ENUM_META || !sel->result_type)
        return false;

    XrType *receiver = sel->receiver_type;
    XrType *owner = xr_type_enum_metadata_owner(receiver);
    XiValue *descriptor = xi_lower_expr(l, ma->object);
    if (!descriptor)
        return true;
    /* Flow narrowing can prove an erased union value to be
     * EnumVariant<E>/EnumPayloadField<E>.  The runtime value is still the
     * explicit erased descriptor box, so recover its scalar only on this
     * statically proven metadata access path.  A typed descriptor remains
     * an I64 and needs no conversion. */
    if (xr_type_is_enum_metadata(receiver) && !xr_type_is_enum_metadata(descriptor->type)) {
        XiValue *unbox = xi_value_new(l->func, l->cur_block, XI_ENUM_DESCRIPTOR_UNBOX, receiver, 1);
        if (!unbox)
            return true;
        unbox->args[0] = descriptor;
        unbox->line = (uint32_t) node->line;
        descriptor = lower_mark_enum_metadata(unbox, owner, (uint8_t) sel->field_index);
    }
    if (sel->field_index == XA_ENUM_META_LENGTH) {
        if (xr_type_is_enum_metadata_named(receiver, XR_ENUM_VARIANTS_TYPE_NAME))
            *out = lower_mark_enum_metadata_from(
                xi_const_int(l->func, l->cur_block,
                             owner && owner->enum_type.layout
                                 ? (int64_t) owner->enum_type.layout->variant_count
                                 : 0,
                             l->type_int),
                owner, XA_ENUM_META_LENGTH, descriptor);
        else {
            XiValue *shift = xi_const_int(l->func, l->cur_block, 32, l->type_int);
            *out = lower_mark_enum_metadata_from(
                xi_binary(l->func, l->cur_block, XI_SHR, l->type_int, descriptor, shift), owner,
                XA_ENUM_META_PAYLOAD_COUNT, descriptor);
        }
        return true;
    }
    if (sel->field_index == XA_ENUM_META_ORDINAL) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, l->type_int, 1);
        if (copy)
            copy->args[0] = descriptor;
        *out = lower_mark_enum_metadata_from(copy, owner, XA_ENUM_META_ORDINAL, descriptor);
        return true;
    }
    if (sel->field_index == XA_ENUM_META_PAYLOAD_INDEX) {
        XiValue *mask = xi_const_int(l->func, l->cur_block, UINT32_MAX, l->type_int);
        *out = lower_mark_enum_metadata_from(
            xi_binary(l->func, l->cur_block, XI_BAND, l->type_int, descriptor, mask), owner,
            XA_ENUM_META_PAYLOAD_INDEX, descriptor);
        return true;
    }

    const char *enum_name =
        owner && owner->kind == XR_KIND_ENUM ? owner->enum_type.enum_name : NULL;
    XaSymbol *enum_sym = sel->target_symbol;
    if (!lower_symbol_has_enum_schema(l, enum_sym) && enum_name)
        enum_sym = xa_analyzer_lookup_deep(l->analyzer, enum_name);
    if (sel->field_index == XA_ENUM_META_PAYLOAD_TYPE) {
        *out = lower_enum_payload_type_id(l, owner, enum_sym, descriptor, (uint32_t) node->line);
        if (!*out)
            l->had_error = true;
        return true;
    }
    XiValue *enum_namespace =
        lower_symbol_has_enum_schema(l, enum_sym)
            ? xi_lower_enum_namespace_value(l, enum_sym, enum_name, (int) node->line)
            : NULL;
    if (!enum_namespace) {
        l->had_error = true;
        return true;
    }

    int meta_field = sel->field_index;
    if (meta_field == XA_ENUM_META_IS_UNIT || meta_field == XA_ENUM_META_PAYLOADS)
        meta_field = XA_ENUM_META_PAYLOAD_COUNT;
    XiValue *meta =
        xi_value_new(l->func, l->cur_block, XI_ENUM_META_GET,
                     meta_field == XA_ENUM_META_PAYLOAD_COUNT ? l->type_int : sel->result_type, 2);
    if (!meta)
        return true;
    meta->args[0] = enum_namespace;
    meta->args[1] = descriptor;
    meta->aux_int = meta_field;
    lower_mark_enum_metadata_from(meta, owner, (uint8_t) meta_field, descriptor);
    meta->flags |= XI_FLAG_READS_MEM | XI_FLAG_MAY_THROW;
    meta->line = (uint32_t) node->line;
    if (sel->field_index == XA_ENUM_META_IS_UNIT) {
        XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        *out = lower_mark_enum_metadata_from(
            xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, meta, zero), owner,
            XA_ENUM_META_IS_UNIT, descriptor);
        return true;
    }
    if (sel->field_index == XA_ENUM_META_PAYLOADS) {
        XiValue *shift = xi_const_int(l->func, l->cur_block, 32, l->type_int);
        XiValue *count_hi = xi_binary(l->func, l->cur_block, XI_SHL, l->type_int, meta, shift);
        XiValue *mask = xi_const_int(l->func, l->cur_block, UINT32_MAX, l->type_int);
        XiValue *ordinal = xi_binary(l->func, l->cur_block, XI_BAND, l->type_int, descriptor, mask);
        *out = lower_mark_enum_metadata_from(
            xi_binary(l->func, l->cur_block, XI_BOR, sel->result_type, count_hi, ordinal), owner,
            XA_ENUM_META_PAYLOADS, descriptor);
        return true;
    }
    *out = meta;
    return true;
}

static XiValue *lower_member_access(XiLower *l, AstNode *node) {
    MemberAccessNode *ma = &node->as.member_access;
    XiSequenceEvidenceIds sequence_ids;
    uint8_t sequence_access_kind = 0;
    if (ma->name && strcmp(ma->name, "length") == 0 &&
        lower_type_has_sequence_evidence(xi_lower_node_type(l, ma->object)))
        sequence_access_kind = XG_SEQ_ACCESS_LENGTH;
    XiSequenceEvidenceKinds sequence_kinds = {
        .sequence_access_kind = sequence_access_kind,
    };
    xi_lower_take_sequence_evidence_ids(l, (uint32_t) node->line, sequence_kinds, &sequence_ids);

    const XaSelection *sel =
        l && l->analyzer && l->analyzer->selection_table
            ? xa_selection_table_get((XaSelectionTable *) l->analyzer->selection_table, node)
            : NULL;
    XiValue *enum_member = NULL;
    if (lower_selected_enum_member_access(l, node, sel, &enum_member))
        return enum_member;

    XiValue *type_member = lower_type_namespace_member(l, ma);
    if (type_member)
        return type_member;

    XiValue *obj = xi_lower_expr(l, ma->object);
    if (!obj)
        return NULL;

    /* Computed property: the analyzer resolved this read to a "get:<prop>"
     * accessor, so it is a call, not a slot read. Emitted before the layout
     * and slot paths below, none of which have a field to offset to. */
    if (sel && sel->kind == XA_SEL_PROPERTY && sel->target_symbol && sel->target_symbol->name) {
        const char *getter = sel->target_symbol->name;
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                         sel->result_type ? sel->result_type : xi_lower_node_type(l, node), 1);
        if (!v)
            return NULL;
        v->args[0] = obj;
        v->aux = (void *) arena_strdup(l->func, getter);
        v->aux_int = (int64_t) xi_lower_method_symbol(l, getter) << 1;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *module_constant = lower_module_member_constant(l, obj, ma->name);
    if (module_constant)
        return module_constant;

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (sel && sel->result_type && sel->result_type->kind != XR_KIND_UNKNOWN &&
        (!result_type || result_type->kind == XR_KIND_UNKNOWN))
        result_type = sel->result_type;

    /* Struct with compile-time layout → XI_AGG_GET (emitter decides
     * whether to stack-allocate or fall back to OP_GETPROP) */
    XrAggregateLayout *slayout = xi_lower_value_struct_layout(l, obj);
    if (slayout) {
        int sidx = xi_lower_struct_field_index(slayout, ma->name);
        if (sidx >= 0) {
            result_type = xi_lower_struct_field_type(l, result_type, slayout, sidx);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_AGG_GET, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->aux = (void *) slayout;
            v->aux_int = sidx;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    /* Json with a complete compile-time field table → direct indexed access.
     * Non-sealed object literals may still grow dynamically, but their
     * declared field indices remain stable. Computed-key object literals
     * have NULL holes in the analyzer field table and use name lookup
     * because codegen compacts only the static named fields. */
    int fidx = json_field_index(obj->type, ma->name);
    if (fidx < 0 && obj->type && XR_TYPE_IS_JSON(obj->type)) {
        uint16_t evidence_fidx = UINT16_MAX;
        if (xi_lower_find_json_direct_field_ordinal(l, ma->name, (uint32_t) node->line,
                                                    XG_JSON_ACCESS_FIELD_GET, &evidence_fidx))
            fidx = (int) evidence_fidx;
    }
    if (fidx >= 0 &&
        !xi_lower_json_access_requires_dynamic_lookup(l, ma->name, (uint32_t) node->line,
                                                      (uint16_t) fidx, XG_JSON_ACCESS_FIELD_GET)) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_GET_F, result_type, 1);
        if (!v)
            return NULL;
        v->args[0] = obj;
        v->aux_int = fidx;
        v->line = (uint32_t) node->line;
        xi_lower_bind_json_access_id(l, v, ma->name, (uint32_t) node->line, (uint16_t) fidx,
                                     XG_JSON_ACCESS_FIELD_GET);
        xi_lower_bind_record_access_id(l, v, ma->name, (uint32_t) node->line, (uint16_t) fidx,
                                       XG_RECORD_ACCESS_FIELD_GET);
        return v;
    }

    /* Tuple `.N` → XI_TUPLE_GET (analyzer has already bounds-checked N).
     * The member name is always a digit run for tuples; if it's not we
     * leave the access alone and let LOAD_FIELD's runtime guard handle
     * the bad code (it can't actually reach here after analyzer rules
     * are enforced, but stays robust if a later refactor introduces an
     * unverified path). */
    if (obj->type && obj->type->kind == XR_KIND_TUPLE && ma->name) {
        bool digits_only = (ma->name[0] != '\0');
        for (const char *p = ma->name; *p && digits_only; p++) {
            if (*p < '0' || *p > '9')
                digits_only = false;
        }
        if (digits_only) {
            long idx = strtol(ma->name, NULL, 10);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->aux_int = idx;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->aux = (void *) arena_strdup(l->func, ma->name);
    v->aux_int = xi_lower_method_symbol(l, ma->name);
    v->line = (uint32_t) node->line;
    /* Ordinary enum value properties participate in the metadata reachability
     * bitmap only when flow preserved a concrete nominal owner.  Legacy/prelude
     * enum phis can carry an anonymous enum-shaped type; tagging that as a
     * concrete enum domain would make the fail-closed descriptor verifier
     * reject otherwise valid `.name`/`.ordinal` code.  Descriptor selections
     * above always carry their concrete owner and remain strict. */
    if (obj->type && obj->type->kind == XR_KIND_ENUM && obj->type->enum_type.enum_name &&
        obj->type->enum_type.layout && ma->name) {
        if (strcmp(ma->name, "name") == 0)
            lower_mark_enum_metadata(v, obj->type, XA_ENUM_META_NAME);
        else if (strcmp(ma->name, "ordinal") == 0)
            lower_mark_enum_metadata(v, obj->type, XA_ENUM_META_ORDINAL);
    }
    xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
    xi_lower_bind_class_field_id(l, v, obj->type, ma->name);
    if (obj->type && XR_TYPE_IS_JSON(obj->type))
        xi_lower_bind_json_access_id(l, v, ma->name, (uint32_t) node->line, UINT16_MAX,
                                     XG_JSON_ACCESS_FIELD_GET);
    return v;
}

static XiValue *lower_member_set_target(XiValue *obj) {
    while (obj && xi_copy_is_identity_alias(obj) && obj->nargs >= 1 && obj->args[0])
        obj = obj->args[0];
    return obj;
}

static bool lower_enum_descriptor_erases_to(const XiValue *value, const XrType *target) {
    if (!value || !target ||
        (!xr_type_is_enum_metadata(value->type) &&
         value->enum_metadata_kind == XR_ENUM_METADATA_NONE))
        return false;
    if (xr_type_is_enum_metadata(target))
        return !xr_type_equals(value->type, (XrType *) (uintptr_t) target);
    return target->kind == XR_KIND_UNION || target->kind == XR_KIND_INTERFACE ||
           target->kind == XR_KIND_JSON || target->kind == XR_KIND_UNKNOWN;
}

static XiValue *lower_enum_descriptor_box_for_boundary(XiLower *l, XiValue *value, XrType *target,
                                                       uint32_t line) {
    if (!lower_enum_descriptor_erases_to(value, target))
        return value;
    XiValue *box = xi_value_new(l->func, l->cur_block, XI_ENUM_DESCRIPTOR_BOX, target, 1);
    if (!box)
        return NULL;
    box->args[0] = value;
    box->line = line;
    box->enum_metadata_owner = value->enum_metadata_owner
                                   ? value->enum_metadata_owner
                                   : xr_type_enum_metadata_owner(value->type);
    box->enum_metadata_kind = value->enum_metadata_kind != XR_ENUM_METADATA_NONE
                                  ? value->enum_metadata_kind
                                  : (uint8_t) xr_type_enum_metadata_kind(value->type);
    box->enum_metadata_field = value->enum_metadata_field;
    return box;
}

static XiValue *lower_member_set(XiLower *l, AstNode *node) {
    MemberSetNode *ms = &node->as.member_set;
    XiValue *obj = xi_lower_expr(l, ms->object);
    XiValue *val = xi_lower_expr(l, ms->value);
    if (!obj || !val)
        return NULL;
    obj = lower_member_set_target(obj);

    XrType *write_type = xi_lower_node_type(l, node);
    val = lower_enum_descriptor_box_for_boundary(l, val, write_type, (uint32_t) node->line);
    if (!val)
        return NULL;

    struct XrType *result_type = val->type;

    /* Computed property: the analyzer resolved this write to a "set:<prop>"
     * accessor, so it is a call, not a store. Emitted before the layout and
     * slot paths, none of which have a field to store into -- the VM used to
     * find the accessor by name at run time and AOT dropped the write. */
    {
        const XaSelection *psel =
            l && l->analyzer && l->analyzer->selection_table
                ? xa_selection_table_get((XaSelectionTable *) l->analyzer->selection_table, node)
                : NULL;
        if (psel && psel->kind == XA_SEL_PROPERTY && psel->target_symbol &&
            psel->target_symbol->name) {
            const char *setter = psel->target_symbol->name;
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->args[1] = val;
            v->aux = (void *) arena_strdup(l->func, setter);
            v->aux_int = (int64_t) xi_lower_method_symbol(l, setter) << 1;
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    /* Struct with compile-time layout → XI_AGG_SET */
    XrAggregateLayout *slayout = xi_lower_value_struct_layout(l, obj);
    if (slayout) {
        int sidx = xi_lower_struct_field_index(slayout, ms->member);
        if (sidx >= 0) {
            val = xi_lower_narrow_for_native_field(l, node, val, slayout->fields[sidx].native_type);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_AGG_SET, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->args[1] = val;
            v->aux = (void *) slayout;
            v->aux_int = sidx;
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    /* Sealed Json with known field → direct indexed store */
    int fidx = json_field_index(obj->type, ms->member);
    if (fidx < 0 && obj->type && XR_TYPE_IS_JSON(obj->type)) {
        uint16_t evidence_fidx = UINT16_MAX;
        if (xi_lower_find_json_direct_field_ordinal(l, ms->member, (uint32_t) node->line,
                                                    XG_JSON_ACCESS_FIELD_SET, &evidence_fidx))
            fidx = (int) evidence_fidx;
    }
    if (fidx >= 0 &&
        !xi_lower_json_access_requires_dynamic_lookup(l, ms->member, (uint32_t) node->line,
                                                      (uint16_t) fidx, XG_JSON_ACCESS_FIELD_SET)) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_SET_F, result_type, 2);
        if (!v)
            return NULL;
        v->args[0] = obj;
        v->args[1] = val;
        v->aux_int = fidx;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        xi_lower_bind_json_access_id(l, v, ms->member, (uint32_t) node->line, (uint16_t) fidx,
                                     XG_JSON_ACCESS_FIELD_SET);
        xi_lower_bind_record_access_id(l, v, ms->member, (uint32_t) node->line, (uint16_t) fidx,
                                       XG_RECORD_ACCESS_FIELD_SET);
        return v;
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, result_type, 2);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->args[1] = val;
    v->aux = (void *) arena_strdup(l->func, ms->member);
    v->aux_int = xi_lower_method_symbol(l, ms->member);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    xi_lower_bind_class_field_id(l, v, obj->type, ms->member);
    if (obj->type && XR_TYPE_IS_JSON(obj->type))
        xi_lower_bind_json_access_id(l, v, ms->member, (uint32_t) node->line, UINT16_MAX,
                                     XG_JSON_ACCESS_FIELD_SET);
    return v;
}

#include "xi_lower_scalar_rep.inc.c"

static bool xi_lower_type_is_unknown(struct XrType *type) {
    return !type || XR_TYPE_IS_UNKNOWN(type);
}

static struct XrType *xi_lower_type_for_native_layout(XiLower *l, struct XrType *fallback,
                                                      uint8_t native_type) {
    if (!l)
        return fallback;
    switch (native_type) {
        case XR_NATIVE_I64:
            return l->type_int ? l->type_int : fallback;
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32: {
            struct XrType *type = xr_type_new_int_width(l->isolate, native_type);
            return type ? type : fallback;
        }
        case XR_NATIVE_F64:
            return l->type_float ? l->type_float : fallback;
        case XR_NATIVE_F32: {
            struct XrType *type = xr_type_new_float_width(l->isolate, native_type);
            return type ? type : fallback;
        }
        case XR_NATIVE_BOOL:
            return l->type_bool ? l->type_bool : fallback;
        case XR_NATIVE_STRING:
            return l->type_string ? l->type_string : fallback;
        default:
            return fallback;
    }
}

static struct XrType *xi_lower_widened_elem_type(XiLower *l, struct XrType *fallback,
                                                 struct XrType *elem_type) {
    if (!elem_type)
        return fallback;
    switch (elem_type->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
            return l && l->type_int ? l->type_int : fallback;
        case XR_NATIVE_F32:
            return l && l->type_float ? l->type_float : fallback;
        default:
            return elem_type;
    }
}

static struct XrType *xi_lower_struct_field_type(XiLower *l, struct XrType *fallback,
                                                 XrAggregateLayout *layout, int field_index) {
    if (!layout || field_index < 0 || field_index >= layout->field_count)
        return fallback;
    XrAggregateFieldLayout *field = &layout->fields[field_index];
    if (field->native_type == XR_NATIVE_ARRAY) {
        struct XrType *elem_type =
            xi_lower_type_for_native_layout(l, NULL, field->elem_native_type);
        if (!elem_type || !l || !l->isolate || field->elem_count == 0)
            return fallback;
        struct XrType *array_type =
            xr_type_new_fixed_array(l->isolate, elem_type, (int) field->elem_count);
        return array_type ? array_type : fallback;
    }
    return xi_lower_type_for_native_layout(l, fallback, field->native_type);
}

/* Byte size of a raw pointer's pointee, for scaling p[i] / p.offset(i). C
 * pointer arithmetic on Ptr<T> advances by sizeof(T), matching `T*`. */
static int64_t xi_pointer_pointee_size(XiLower *l, struct XrType *ptr_type) {
    if (!ptr_type || !XR_TYPE_IS_POINTER(ptr_type))
        return 1;
    struct XrType *pointee = ptr_type->container.element_type;
    if (!pointee)
        return 1;
    if (pointee->kind == XR_KIND_INT || pointee->kind == XR_KIND_FLOAT)
        return (int64_t) xr_native_type_size(xi_lower_target_data_layout(l), pointee->scalar_rep);
    switch (pointee->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
            return 8;
        case XR_KIND_POINTER:
            return (int64_t) xi_lower_target_data_layout(l)->pointer.size;
        case XR_KIND_BOOL:
            return 1;
        default:
            return 1;
    }
}

static bool xi_type_is_pod_span_elem(struct XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool xi_lower_builtin_receiver_registry_matches(struct XrType *receiver_type,
                                                       XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver_type && receiver_type->kind == XR_KIND_INT &&
                   !receiver_type->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver_type);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver_type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver_type && XR_TYPE_IS_ARRAY(receiver_type);
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver_type);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver_type && XR_TYPE_IS_SLICE(receiver_type) &&
                   xi_type_is_pod_span_elem(receiver_type->container.element_type);
    }
    return false;
}

/* R2-2: wrappingAdd/Sub/Mul are spec-defined as "explicit default two's-
 * complement wrap" — exactly the semantics ADD/SUB/MUL already implement per
 * receiver width (§2.3: fixed-width arithmetic wraps at its width). Lowering
 * the method to the operator + the receiver-width narrow makes the family
 * width-exact on every int receiver and on both backends; the runtime method
 * fallback computes at int64 and would silently lose the width. */
static uint16_t xi_lower_int_wrapping_method_op(struct XrType *receiver_type, const char *method,
                                                int arg_count) {
    if (arg_count != 1 || !method || !receiver_type || receiver_type->kind != XR_KIND_INT ||
        receiver_type->is_nullable)
        return XI_OP_COUNT;
    if (strcmp(method, "wrappingAdd") == 0)
        return XI_ADD;
    if (strcmp(method, "wrappingSub") == 0)
        return XI_SUB;
    if (strcmp(method, "wrappingMul") == 0)
        return XI_MUL;
    return XI_OP_COUNT;
}

static XiValue *xi_lower_int_wrapping_method(XiLower *l, AstNode *node, uint16_t op, XiValue *recv,
                                             XiValue *arg) {
    /* Same IR shape as `recv <op> arg` on the receiver's type: the binary op
     * followed by the width narrow (no-op for int/int64). Truncating the
     * int64 result is exact for every receiver width and any int arg width:
     * (a op b) mod 2^64 then mod 2^w == (a op b) mod 2^w. */
    XiValue *raw = xi_binary(l->func, l->cur_block, op, recv->type, recv, arg);
    if (!raw)
        return NULL;
    raw->line = (uint32_t) node->line;
    return xi_lower_wrap_if_needed(l, node, raw, recv->type, op);
}

static bool xi_lower_receiver_method_arg_count_matches(const XaBuiltinReceiverMethodSpec *spec,
                                                       int arg_count) {
    if (!spec || arg_count < spec->min_params)
        return false;
    if (spec->is_variadic)
        return true;
    return arg_count <= spec->param_count;
}

static bool xi_lower_receiver_method_call_matches(struct XrType *receiver_type, const char *method,
                                                  int arg_count,
                                                  XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    return spec && method && strcmp(method, spec->source_name) == 0 &&
           xi_lower_builtin_receiver_registry_matches(receiver_type, spec->receiver) &&
           xi_lower_receiver_method_arg_count_matches(spec, arg_count);
}

static bool xi_lower_receiver_method_call_matches_either(struct XrType *receiver_type,
                                                         const char *method, int arg_count,
                                                         XaBuiltinReceiverMethodId left,
                                                         XaBuiltinReceiverMethodId right) {
    return xi_lower_receiver_method_call_matches(receiver_type, method, arg_count, left) ||
           xi_lower_receiver_method_call_matches(receiver_type, method, arg_count, right);
}

static XrArrayElemType xi_pod_span_elem_type(struct XrType *type) {
    if (!xi_type_is_pod_span_elem(type))
        return XR_ELEM_ANY;
    switch (type->kind) {
        case XR_KIND_INT:
            switch (type->scalar_rep) {
                case XR_NATIVE_I8:
                    return XR_ELEM_I8;
                case XR_NATIVE_U8:
                    return XR_ELEM_U8;
                case XR_NATIVE_I16:
                    return XR_ELEM_I16;
                case XR_NATIVE_U16:
                    return XR_ELEM_U16;
                case XR_NATIVE_I32:
                    return XR_ELEM_I32;
                case XR_NATIVE_U32:
                    return XR_ELEM_U32;
                case XR_NATIVE_U64:
                case XR_NATIVE_USIZE:
                    return XR_ELEM_U64;
                case XR_NATIVE_ISIZE:
                    return XR_ELEM_I64;
                default:
                    return XR_ELEM_I64;
            }
        case XR_KIND_FLOAT:
            return type->scalar_rep == XR_NATIVE_F32 ? XR_ELEM_F32 : XR_ELEM_F64;
        case XR_KIND_BOOL:
            return XR_ELEM_BOOL;
        case XR_KIND_RUNE:
            return XR_ELEM_RUNE;
        default:
            return XR_ELEM_ANY;
    }
}

static int64_t xi_pack_span_elem_aux(XiLower *l, struct XrType *type) {
    uint32_t elem_size = 0;
    uint32_t alignment = 0;
    if (!l || !type ||
        !xr_type_has_static_layout(xi_lower_target_data_layout(l), type, &elem_size, &alignment) ||
        !xr_type_all_bit_patterns_valid(type) || elem_size == 0 || elem_size > UINT16_MAX ||
        alignment == 0 || alignment > UINT16_MAX)
        return 0;
    XrArrayElemType elem_type = xi_pod_span_elem_type(type);
    if (elem_type >= XR_ELEM_COUNT)
        elem_type = XR_ELEM_ANY;
    uint8_t elem_tid = xr_type_to_tid(type);
    return (int64_t) ((uint8_t) elem_type) | ((int64_t) elem_size << 8) |
           ((int64_t) elem_tid << 24) | ((int64_t) alignment << 32);
}

static int64_t xi_pack_slice_from_ptr_aux(XiLower *l, struct XrType *type) {
    uint32_t elem_size = 0;
    uint32_t alignment = 0;
    if (!l || !type ||
        !xr_type_has_static_layout(xi_lower_target_data_layout(l), type, &elem_size, &alignment) ||
        elem_size == 0 || elem_size > INT16_MAX || alignment == 0 || alignment > INT16_MAX)
        return 0;
    XrArrayElemType elem_type = xi_pod_span_elem_type(type);
    uint8_t elem_tid = xr_type_to_tid(type);
    return (int64_t) ((uint8_t) elem_type) | ((int64_t) elem_size << 8) |
           ((int64_t) elem_tid << 24) | ((int64_t) alignment << 32);
}

/* XrFFIType width code of a raw pointer's pointee (carried on PTR_LOAD/STORE). */
static uint8_t xi_pointer_pointee_ffi(struct XrType *ptr_type) {
    struct XrType *pointee =
        (ptr_type && XR_TYPE_IS_POINTER(ptr_type)) ? ptr_type->container.element_type : NULL;
    return xr_ffi_type_from_xrtype(pointee, false);
}

/* Build the scaled address `ptr + idx * sizeof(pointee)` as a raw-pointer SSA
 * value. VM/tagged boundaries still encode the address as an integer, but AOT
 * keeps the local as a native pointer. */
static XiValue *xi_lower_ptr_scaled_addr(XiLower *l, AstNode *node, XiValue *ptr, XiValue *idx,
                                         struct XrType *ptr_type, struct XrType *addr_type) {
    XiValue *scaled = idx;
    int64_t size = xi_pointer_pointee_size(l, ptr_type);
    if (size != 1) {
        XiValue *sz = xi_const_int(l->func, l->cur_block, size, l->type_int);
        XiValue *mul = xi_value_new(l->func, l->cur_block, XI_MUL, l->type_int, 2);
        if (!mul)
            return NULL;
        mul->args[0] = idx;
        mul->args[1] = sz;
        mul->line = (uint32_t) node->line;
        scaled = mul;
    }
    XiValue *add = xi_value_new(l->func, l->cur_block, XI_ADD, addr_type, 2);
    if (!add)
        return NULL;
    add->args[0] = ptr;
    add->args[1] = scaled;
    add->line = (uint32_t) node->line;
    return add;
}

/* Lower `unsafe { stmt* }`: run the statements; the value is the trailing
 * expression statement (or null). unsafe is otherwise codegen-transparent. */
static XiValue *lower_unsafe_expr(XiLower *l, AstNode *node) {
    AstNode *body = node->as.unsafe_expr.operand;
    if (!body)
        return xi_const_null(l->func, l->cur_block, l->type_null);
    if (body->type != AST_BLOCK)
        return xi_lower_expr(l, body);
    BlockNode *blk = &body->as.block;
    XiValue *value = NULL;
    for (int i = 0; i < blk->count; i++) {
        AstNode *stmt = blk->statements[i];
        if (!stmt)
            continue;
        bool is_last = (i == blk->count - 1);
        if (is_last && stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
            value = xi_lower_expr(l, stmt->as.expr_stmt);
            if (!l->cur_block)
                return NULL;
        } else {
            xi_lower_stmt(l, stmt);
            if (!l->cur_block)
                return NULL;
        }
    }
    if (!value)
        value = xi_const_null(l->func, l->cur_block, l->type_null);
    return value;
}

static XiValue *lower_index_get(XiLower *l, AstNode *node) {
    IndexGetNode *ig = &node->as.index_get;
    XiSequenceEvidenceIds sequence_ids;
    uint8_t sequence_access_kind =
        lower_type_has_sequence_evidence(xi_lower_node_type(l, ig->array)) ? XG_SEQ_ACCESS_INDEX_GET
                                                                           : 0;
    XiSequenceEvidenceKinds sequence_kinds = {
        .sequence_access_kind = sequence_access_kind,
    };
    xi_lower_take_sequence_evidence_ids(l, (uint32_t) node->line, sequence_kinds, &sequence_ids);
    uint32_t key_access_ordinal =
        xi_lower_next_key_access_ordinal(l, (uint32_t) node->line, XG_KEY_ACCESS_INDEX_GET);
    XiValue *obj = xi_lower_expr(l, ig->array);
    XiValue *idx = xi_lower_expr(l, ig->index);
    if (!obj || !idx)
        return NULL;

    if (xr_type_is_enum_metadata_named(obj->type, XR_ENUM_VARIANTS_TYPE_NAME) ||
        xr_type_is_enum_metadata_named(obj->type, XR_ENUM_PAYLOADS_TYPE_NAME)) {
        struct XrType *result_type = xi_lower_node_type(l, node);
        XiValue *v =
            xi_value_new(l->func, l->cur_block,
                         xr_type_is_enum_metadata_named(obj->type, XR_ENUM_VARIANTS_TYPE_NAME)
                             ? XI_ENUM_VARIANT_AT
                             : XI_ENUM_PAYLOAD_AT,
                         result_type, 2);
        if (!v)
            return NULL;
        v->args[0] = obj;
        v->args[1] = idx;
        v->flags |= XI_FLAG_MAY_THROW;
        v->line = (uint32_t) node->line;
        v->enum_metadata_owner = xr_type_enum_metadata_owner(result_type);
        v->enum_metadata_kind =
            (uint8_t) (v->op == XI_ENUM_VARIANT_AT ? XR_ENUM_METADATA_VARIANT
                                                   : XR_ENUM_METADATA_PAYLOAD_FIELD);
        return v;
    }

    /* FFI raw pointer subscript p[i] => XI_PTR_LOAD(p + i*sizeof(T)). */
    if (obj->type && XR_TYPE_IS_POINTER(obj->type)) {
        struct XrType *result_type = obj->type->container.element_type;
        if (!result_type || XR_TYPE_IS_UNKNOWN(result_type))
            result_type = xi_lower_node_type(l, node);
        XiValue *addr = xi_lower_ptr_scaled_addr(l, node, obj, idx, obj->type, obj->type);
        if (!addr)
            return NULL;
        XiValue *endian = xi_const_int(l->func, l->cur_block, XR_ENDIAN_NATIVE, l->type_int);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, result_type, 2);
        if (!v)
            return NULL;
        v->args[0] = addr;
        v->args[1] = endian;
        v->aux_int = (int64_t) xr_ffi_ptr_aux(xi_pointer_pointee_ffi(obj->type), false);
        v->flags |= XI_FLAG_READS_MEM;
        v->line = (uint32_t) node->line;
        return v;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (obj->type && XR_TYPE_IS_JSON(obj->type)) {
        const char *static_key = lower_static_string_key(ig->index);
        uint16_t evidence_fidx = UINT16_MAX;
        if (static_key &&
            xi_lower_find_json_direct_field_ordinal(l, static_key, (uint32_t) node->line,
                                                    XG_JSON_ACCESS_INDEX_GET, &evidence_fidx)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_GET_F, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->aux_int = evidence_fidx;
            v->line = (uint32_t) node->line;
            xi_lower_bind_json_access_id(l, v, static_key, (uint32_t) node->line, evidence_fidx,
                                         XG_JSON_ACCESS_INDEX_GET);
            return v;
        }
    }
    if (obj->type && XR_TYPE_IS_MAP(obj->type))
        idx = xi_lower_narrow_for_static_type(l, node, idx, obj->type->map.key_type);
    struct XrType *elem_type = xi_get_container_elem_type(obj->type);
    struct XrType *index_type =
        xi_lower_type_is_unknown(result_type) && elem_type ? elem_type : result_type;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, index_type, 2);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->line = (uint32_t) node->line;
    xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
    if (obj->type && XR_TYPE_IS_JSON(obj->type)) {
        const char *static_key = lower_static_string_key(ig->index);
        xi_lower_bind_json_access_id(l, v, static_key, (uint32_t) node->line, UINT16_MAX,
                                     XG_JSON_ACCESS_INDEX_GET);
    }
    xi_lower_bind_key_access_id(l, v, (uint32_t) node->line, key_access_ordinal,
                                XG_KEY_ACCESS_INDEX_GET);

    /* Insert XI_WIDEN after reading from a sub-width typed array */
    uint16_t widen_op = xi_widen_op_for_elem(elem_type);
    if (widen_op) {
        struct XrType *widen_type = xi_lower_type_is_unknown(result_type)
                                        ? xi_lower_widened_elem_type(l, result_type, elem_type)
                                        : result_type;
        XiValue *w = xi_value_new(l->func, l->cur_block, widen_op, widen_type, 1);
        if (!w)
            return v;
        w->args[0] = v;
        w->line = (uint32_t) node->line;
        return w;
    }
    return v;
}

static XiValue *lower_index_set(XiLower *l, AstNode *node) {
    IndexSetNode *is_node = &node->as.index_set;
    XiSequenceEvidenceIds sequence_ids;
    uint8_t sequence_access_kind =
        lower_type_has_sequence_evidence(xi_lower_node_type(l, is_node->array))
            ? XG_SEQ_ACCESS_INDEX_SET
            : 0;
    XiSequenceEvidenceKinds sequence_kinds = {
        .sequence_access_kind = sequence_access_kind,
    };
    xi_lower_take_sequence_evidence_ids(l, (uint32_t) node->line, sequence_kinds, &sequence_ids);
    uint32_t key_access_ordinal =
        xi_lower_next_key_access_ordinal(l, (uint32_t) node->line, XG_KEY_ACCESS_SET);
    XiValue *obj = xi_lower_expr(l, is_node->array);
    XiValue *idx = xi_lower_expr(l, is_node->index);
    XiValue *val = xi_lower_expr(l, is_node->value);
    if (!obj || !idx || !val)
        return NULL;

    /* FFI raw pointer store p[i] = v => XI_PTR_STORE(p + i*sizeof(T), v). */
    if (obj->type && XR_TYPE_IS_POINTER(obj->type)) {
        struct XrType *pointee_type = obj->type->container.element_type;
        val = xi_lower_narrow_for_static_type(l, node, val, pointee_type);
        XiValue *addr = xi_lower_ptr_scaled_addr(l, node, obj, idx, obj->type, obj->type);
        if (!addr)
            return NULL;
        XiValue *endian = xi_const_int(l->func, l->cur_block, XR_ENDIAN_NATIVE, l->type_int);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_STORE, l->type_unit, 3);
        if (!v)
            return NULL;
        v->args[0] = addr;
        v->args[1] = val;
        v->args[2] = endian;
        v->aux_int = (int64_t) xr_ffi_ptr_aux(xi_pointer_pointee_ffi(obj->type), false);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
        v->line = (uint32_t) node->line;
        return v;
    }

    if (obj->type && XR_TYPE_IS_JSON(obj->type)) {
        const char *static_key = lower_static_string_key(is_node->index);
        uint16_t evidence_fidx = UINT16_MAX;
        if (static_key &&
            xi_lower_find_json_direct_field_ordinal(l, static_key, (uint32_t) node->line,
                                                    XG_JSON_ACCESS_INDEX_SET, &evidence_fidx)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_SET_F, val->type, 2);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->args[1] = val;
            v->aux_int = evidence_fidx;
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            xi_lower_bind_json_access_id(l, v, static_key, (uint32_t) node->line, evidence_fidx,
                                         XG_JSON_ACCESS_INDEX_SET);
            return v;
        }
    }

    if (obj->type && XR_TYPE_IS_MAP(obj->type)) {
        idx = xi_lower_narrow_for_static_type(l, node, idx, obj->type->map.key_type);
        val = xi_lower_narrow_for_static_type(l, node, val, obj->type->map.value_type);
    } else {
        /* Insert XI_NARROW before writing to a sub-width typed array */
        struct XrType *elem_type = xi_get_container_elem_type(obj->type);
        uint16_t narrow_op = xi_narrow_op_for_elem(elem_type);
        if (narrow_op) {
            XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, val->type, 1);
            if (n) {
                n->args[0] = val;
                n->line = (uint32_t) node->line;
                val = n;
            }
        }
    }

    struct XrType *result_type = val->type;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, result_type, 3);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->args[2] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
    if (obj->type && XR_TYPE_IS_JSON(obj->type)) {
        const char *static_key = lower_static_string_key(is_node->index);
        xi_lower_bind_json_access_id(l, v, static_key, (uint32_t) node->line, UINT16_MAX,
                                     XG_JSON_ACCESS_INDEX_SET);
    }
    xi_lower_bind_key_access_id(l, v, (uint32_t) node->line, key_access_ordinal, XG_KEY_ACCESS_SET);
    return v;
}

static XiValue *lower_tuple_literal(XiLower *l, AstNode *node) {
    TupleLiteralNode *tup = &node->as.tuple_literal;
    struct XrType *result_type = xi_lower_node_type(l, node);

    /* First pass: evaluate every element value, expanding spreads into
     * one TUPLE_GET per source slot. The flat list `elem_vals[]`
     * mirrors the final tuple's element layout exactly. */
    XiValue *elem_vals[64];
    uint16_t slot = 0;
    for (int i = 0; i < tup->count && slot < 64; i++) {
        AstNode *child = tup->elements[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return NULL;
            int arity = src->type ? xr_type_tuple_count(src->type) : 0;
            for (int j = 0; j < arity && slot < 64; j++) {
                struct XrType *et = xr_type_tuple_get(src->type, j);
                XiValue *get =
                    xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et ? et : l->type_any, 1);
                if (!get)
                    return NULL;
                get->args[0] = src;
                get->aux_int = j;
                elem_vals[slot++] = get;
            }
            continue;
        }

        elem_vals[slot] = xi_lower_expr(l, child);
        if (!elem_vals[slot])
            return NULL;
        struct XrType *elem_type = result_type && XR_TYPE_IS_TUPLE(result_type)
                                       ? xr_type_tuple_get(result_type, (int) slot)
                                       : NULL;
        elem_vals[slot] =
            xi_lower_apply_numeric_conversion_witness(l, child, elem_vals[slot], elem_type);
        if (!elem_vals[slot])
            return NULL;
        slot++;
    }
    uint16_t safe_n = slot;

    XiValue *tup_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_NEW, result_type, safe_n);
    if (!tup_val)
        return NULL;
    for (uint16_t i = 0; i < safe_n; i++)
        tup_val->args[i] = elem_vals[i];
    tup_val->aux_int = safe_n;
    tup_val->line = (uint32_t) node->line;
    return tup_val;
}

/* Array literal with `...spread` elements: `[...a, x, ...b]`.
 * Built dynamically because spread sources have runtime length — a fresh
 * array is allocated (heap; it grows), singletons are appended with
 * XI_ARRAY_PUSH and each spread source is spliced with XI_ARRAY_EXTEND.
 * Runtime cost is O(total elements). The no-spread path keeps the static
 * pre-sized ARRAY_NEW + INDEX_SET fast path. */
static XiValue *lower_array_literal_spread(XiLower *l, AstNode *node, struct XrType *result_type) {
    ArrayLiteralNode *arr = &node->as.array_literal;
    int count = arr->count;
    struct XrType *elem_type = xi_get_container_elem_type(result_type);

    /* XI_ARRAY_NEW's argument is the initial LENGTH (both backends preset
     * length and fill slots; the static literal path overwrites them via
     * INDEX_SET). The spread path appends everything with PUSH/EXTEND, so
     * it must start from an empty array — a non-zero count here would leave
     * phantom leading null/zero elements. */
    XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
    if (!arr_val)
        return NULL;
    arr_val->args[0] = cap;
    arr_val->line = (uint32_t) node->line;

    for (int i = 0; i < count; i++) {
        AstNode *child = arr->elements[i];
        if (!child)
            continue;
        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return NULL;
            XiValue *ext = xi_value_new(l->func, l->cur_block, XI_ARRAY_EXTEND, l->type_unit, 2);
            if (!ext)
                return NULL;
            ext->args[0] = arr_val;
            ext->args[1] = src;
            ext->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
            ext->line = (uint32_t) node->line;
        } else {
            XiValue *elem = xi_lower_expr(l, child);
            if (!elem)
                return NULL;
            elem = xi_lower_apply_numeric_conversion_witness(l, child, elem, elem_type);
            if (!elem)
                return NULL;
            XiValue *push = xi_value_new(l->func, l->cur_block, XI_ARRAY_PUSH, l->type_unit, 2);
            if (!push)
                return NULL;
            push->args[0] = arr_val;
            push->args[1] = elem;
            push->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            push->line = (uint32_t) node->line;
        }
    }
    return arr_val;
}

static int xi_fixed_array_elem_native_type(struct XrType *type) {
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || !type->fixed_array.element_type ||
        type->fixed_array.length <= 0)
        return -1;
    XrType *elem = type->fixed_array.element_type;
    if (elem->is_nullable)
        return XR_NATIVE_VALUE;
    int native = xr_type_kind_to_native(elem->kind, elem->scalar_rep);
    if (native == XR_NATIVE_I64 || native == XR_NATIVE_F64 || native == XR_NATIVE_BOOL ||
        native == XR_NATIVE_I8 || native == XR_NATIVE_I16 || native == XR_NATIVE_I32 ||
        native == XR_NATIVE_U8 || native == XR_NATIVE_U16 || native == XR_NATIVE_U32 ||
        native == XR_NATIVE_U64 || native == XR_NATIVE_ISIZE || native == XR_NATIVE_USIZE ||
        native == XR_NATIVE_F32)
        return native;
    if (native == XR_NATIVE_STRING || native < 0)
        return XR_NATIVE_VALUE;
    return native;
}

static XiValue *lower_fixed_array_store_elem(XiLower *l, AstNode *node, XiValue *arr_val, int idx,
                                             XiValue *elem, struct XrType *elem_type, int native) {
    if (!l || !arr_val || !elem)
        return NULL;
    XiValue *idx_val = xi_const_int(l->func, l->cur_block, idx, l->type_int);
    if (!idx_val)
        return NULL;
    if (native != XR_NATIVE_VALUE) {
        uint16_t narrow_op = xi_narrow_op_for_elem(elem_type);
        if (narrow_op) {
            XiValue *narrow = xi_value_new(l->func, l->cur_block, narrow_op, elem->type, 1);
            if (narrow) {
                narrow->args[0] = elem;
                narrow->line = (uint32_t) node->line;
                elem = narrow;
            }
        }
    }
    XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
    if (!set)
        return NULL;
    set->args[0] = arr_val;
    set->args[1] = idx_val;
    set->args[2] = elem;
    set->flags |= XI_FLAG_SIDE_EFFECT;
    set->line = (uint32_t) node->line;
    return set;
}

static bool xi_fixed_array_repeat_value_is_zero(AstNode *node, int native) {
    if (!node || native == XR_NATIVE_VALUE)
        return false;
    if (node->type == AST_LITERAL_INT)
        return node->as.literal.raw_value.int_val == 0;
    if (node->type == AST_LITERAL_FALSE)
        return true;
    if (node->type == AST_LITERAL_FLOAT)
        return node->as.literal.raw_value.float_val == 0.0;
    return false;
}

static XiValue *lower_fixed_array_literal(XiLower *l, AstNode *node, ArrayLiteralNode *arr,
                                          struct XrType *result_type) {
    if (!result_type || result_type->kind != XR_KIND_FIXED_ARRAY)
        return NULL;
    int native = xi_fixed_array_elem_native_type(result_type);
    if (native < 0) {
        fprintf(stderr, "[LOWER] fixed array element type '%s' has no native layout at line %d\n",
                result_type->fixed_array.element_type
                    ? xr_type_to_string(result_type->fixed_array.element_type)
                    : "?",
                (int) node->line);
        l->had_error = true;
        return NULL;
    }
    int expected_count = result_type->fixed_array.length;
    if (arr->is_repeat) {
        XiValue *repeat_value = xi_lower_expr(l, arr->repeat_value);
        if (!repeat_value)
            return NULL;
        repeat_value = xi_lower_apply_numeric_conversion_witness(
            l, arr->repeat_value, repeat_value, result_type->fixed_array.element_type);
        if (!repeat_value)
            return NULL;
        XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_FIXED_ARRAY_NEW, result_type, 0);
        if (!arr_val)
            return NULL;
        arr_val->aux_int = native;
        arr_val->flags |= XI_FLAG_SIDE_EFFECT;
        arr_val->line = (uint32_t) node->line;

        if (xi_fixed_array_repeat_value_is_zero(arr->repeat_value, native))
            return arr_val;

        for (int i = 0; i < expected_count; i++) {
            if (!lower_fixed_array_store_elem(l, node, arr_val, i, repeat_value,
                                              result_type->fixed_array.element_type, native))
                return NULL;
        }
        return arr_val;
    }

    if (arr->count != expected_count) {
        fprintf(stderr,
                "[LOWER] fixed array literal length mismatch at line %d: got %d, expected %d\n",
                (int) node->line, arr->count, expected_count);
        l->had_error = true;
        return NULL;
    }

    int n = arr->count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, arr->elements[i]);
        if (!elem_vals[i])
            return NULL;
        elem_vals[i] = xi_lower_apply_numeric_conversion_witness(
            l, arr->elements[i], elem_vals[i], result_type->fixed_array.element_type);
        if (!elem_vals[i])
            return NULL;
    }

    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_FIXED_ARRAY_NEW, result_type, 0);
    if (!arr_val)
        return NULL;
    arr_val->aux_int = native;
    arr_val->flags |= XI_FLAG_SIDE_EFFECT;
    arr_val->line = (uint32_t) node->line;

    for (int i = 0; i < n; i++) {
        if (!lower_fixed_array_store_elem(l, node, arr_val, i, elem_vals[i],
                                          result_type->fixed_array.element_type, native))
            return NULL;
    }
    return arr_val;
}

static XiValue *lower_static_bytes_literal_ptr(XiLower *l, AstNode *call_node,
                                               AstNode *literal_node) {
    if (!l || !call_node || !literal_node || literal_node->type != AST_FIXED_BYTES_LITERAL)
        return NULL;

    FixedBytesLiteralNode *literal = &literal_node->as.fixed_bytes_literal;
    size_t payload_length = literal->payload_length;
    size_t total_length = payload_length + (literal->append_nul ? 1u : 0u);
    if (total_length > INT_MAX) {
        l->had_error = true;
        return NULL;
    }

    int count = (int) total_length;
    uint32_t alloc_size = (uint32_t) (count > 0 ? count : 1);
    uint8_t *bytes = (uint8_t *) xi_func_arena_alloc(l->func, alloc_size);
    if (!bytes)
        return NULL;
    if (payload_length > 0)
        memcpy(bytes, literal->payload, payload_length);
    if (literal->append_nul)
        bytes[payload_length] = 0;
    else if (count == 0)
        bytes[0] = 0;

    struct XrType *result_type = xi_lower_node_type(l, call_node);
    XiValue *address = xi_value_new(l->func, l->cur_block, XI_STATIC_BYTES_PTR, result_type, 0);
    if (!address)
        return NULL;
    address->aux = bytes;
    address->aux_int = count;
    address->line = (uint32_t) call_node->line;
    return address;
}

static XiValue *lower_fixed_bytes_literal(XiLower *l, AstNode *node) {
    if (!l || !node || node->type != AST_FIXED_BYTES_LITERAL)
        return NULL;
    FixedBytesLiteralNode *literal = &node->as.fixed_bytes_literal;
    size_t total_length = literal->payload_length + (literal->append_nul ? 1u : 0u);
    if (total_length > INT_MAX) {
        l->had_error = true;
        return NULL;
    }
    uint32_t alloc_size = (uint32_t) (total_length > 0 ? total_length : 1);
    uint8_t *bytes = (uint8_t *) xi_func_arena_alloc(l->func, alloc_size);
    if (!bytes)
        return NULL;
    if (literal->payload_length > 0)
        memcpy(bytes, literal->payload, literal->payload_length);
    if (literal->append_nul)
        bytes[literal->payload_length] = 0;
    else if (total_length == 0)
        bytes[0] = 0;

    XiValue *value =
        xi_value_new(l->func, l->cur_block, XI_FIXED_BYTES_CONST, xi_lower_node_type(l, node), 0);
    if (!value)
        return NULL;
    value->aux = bytes;
    value->aux_int = (int64_t) total_length;
    value->flags |= XI_FLAG_SIDE_EFFECT;
    value->line = (uint32_t) node->line;
    return value;
}

static XiValue *lower_array_literal(XiLower *l, AstNode *node) {
    ArrayLiteralNode *arr = &node->as.array_literal;
    int count = arr->count;
    struct XrType *result_type = xi_lower_node_type(l, node);

    if (result_type && result_type->kind == XR_KIND_FIXED_ARRAY)
        return lower_fixed_array_literal(l, node, arr, result_type);

    /* Spread elements force the dynamic build path. */
    for (int i = 0; i < count; i++) {
        if (arr->elements[i] && arr->elements[i]->type == AST_SPREAD_EXPR)
            return lower_array_literal_spread(l, node, result_type);
    }

    /* Evaluate all elements first */
    int n = count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, arr->elements[i]);
        if (!elem_vals[i])
            return NULL;
        elem_vals[i] = xi_lower_apply_numeric_conversion_witness(
            l, arr->elements[i], elem_vals[i], xi_get_container_elem_type(result_type));
        if (!elem_vals[i])
            return NULL;
    }

    /* Create array: XI_ARRAY_NEW with element count as aux */
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
    if (!arr_val)
        return NULL;
    arr_val->args[0] = cap;
    arr_val->line = (uint32_t) node->line;

    /* Populate: INDEX_SET for each element */
    struct XrType *elem_type = xi_get_container_elem_type(result_type);
    uint16_t narrow_op = xi_narrow_op_for_elem(elem_type);
    for (int i = 0; i < n; i++) {
        XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
        XiValue *elem = elem_vals[i];
        if (narrow_op) {
            XiValue *narrow = xi_value_new(l->func, l->cur_block, narrow_op, elem->type, 1);
            if (narrow) {
                narrow->args[0] = elem;
                narrow->line = (uint32_t) node->line;
                elem = narrow;
            }
        }
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
        if (!set)
            break;
        set->args[0] = arr_val;
        set->args[1] = idx;
        set->args[2] = elem;
        set->flags |= XI_FLAG_SIDE_EFFECT;
        set->line = (uint32_t) node->line;
    }
    return arr_val;
}

static XiValue *lower_ct_value(XiLower *l, AstNode *node, const XrCtValue *value,
                               struct XrType *target_type);

static XiValue *lower_ct_tuple_value(XiLower *l, AstNode *node, const XrCtValue *value,
                                     struct XrType *target_type) {
    if (!l || !node || !value || value->kind != XR_CT_TUPLE)
        return NULL;
    int count = value->as.tuple_val.count;
    if (count < 0 || count > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] comptime tuple value arity exceeds %d at line %d\n",
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }
    if (target_type && target_type->kind == XR_KIND_TUPLE &&
        xr_type_tuple_count(target_type) != count) {
        fprintf(stderr, "[LOWER] comptime tuple value arity mismatch at line %d\n",
                (int) node->line);
        l->had_error = true;
        return NULL;
    }

    int alloc_n = count > 0 ? count : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < count; i++) {
        struct XrType *elem_type = (target_type && target_type->kind == XR_KIND_TUPLE)
                                       ? xr_type_tuple_get(target_type, i)
                                       : NULL;
        elem_vals[i] =
            lower_ct_value(l, node, &value->as.tuple_val.elements[i], elem_type ? elem_type : NULL);
        if (!elem_vals[i])
            return NULL;
    }

    XiValue *tup_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_NEW,
                                    target_type ? target_type : l->type_any, (uint16_t) count);
    if (!tup_val)
        return NULL;
    for (uint16_t i = 0; i < (uint16_t) count; i++)
        tup_val->args[i] = elem_vals[i];
    tup_val->aux_int = count;
    tup_val->line = (uint32_t) node->line;
    return tup_val;
}

static XiValue *lower_ct_fixed_array_value(XiLower *l, AstNode *node, const XrCtValue *value,
                                           struct XrType *target_type) {
    if (!l || !node || !value || value->kind != XR_CT_FIXED_ARRAY)
        return NULL;
    if (!target_type || target_type->kind != XR_KIND_FIXED_ARRAY) {
        fprintf(stderr,
                "[LOWER] comptime fixed-array value needs fixed-array target type at line "
                "%d\n",
                (int) node->line);
        l->had_error = true;
        return NULL;
    }
    int count = value->as.fixed_array_val.count;
    if (count != target_type->fixed_array.length) {
        fprintf(stderr,
                "[LOWER] comptime fixed-array value length mismatch at line %d: got %d, expected "
                "%d\n",
                (int) node->line, count, target_type->fixed_array.length);
        l->had_error = true;
        return NULL;
    }
    int native = xi_fixed_array_elem_native_type(target_type);
    if (native < 0) {
        fprintf(stderr,
                "[LOWER] comptime fixed-array element type '%s' has no native layout at "
                "line %d\n",
                target_type->fixed_array.element_type
                    ? xr_type_to_string(target_type->fixed_array.element_type)
                    : "?",
                (int) node->line);
        l->had_error = true;
        return NULL;
    }

    if (value->as.fixed_array_val.is_byte_blob) {
        if (native != XR_NATIVE_U8 || (count > 0 && !value->as.fixed_array_val.byte_blob)) {
            fprintf(stderr, "[LOWER] compact fixed bytes require byte element type at line %d\n",
                    (int) node->line);
            l->had_error = true;
            return NULL;
        }
        uint8_t *bytes =
            (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) (count > 0 ? count : 1));
        if (!bytes)
            return NULL;
        if (count > 0)
            memcpy(bytes, value->as.fixed_array_val.byte_blob, (size_t) count);
        else
            bytes[0] = 0;
        XiValue *compact =
            xi_value_new(l->func, l->cur_block, XI_FIXED_BYTES_CONST, target_type, 0);
        if (!compact)
            return NULL;
        compact->aux = bytes;
        compact->aux_int = count;
        compact->flags |= XI_FLAG_SIDE_EFFECT;
        compact->line = (uint32_t) node->line;
        return compact;
    }

    int alloc_n = count > 0 ? count : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < count; i++) {
        elem_vals[i] = lower_ct_value(l, node, &value->as.fixed_array_val.elements[i],
                                      target_type->fixed_array.element_type);
        if (!elem_vals[i])
            return NULL;
    }

    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_FIXED_ARRAY_NEW, target_type, 0);
    if (!arr_val)
        return NULL;
    arr_val->aux_int = native;
    arr_val->flags |= XI_FLAG_SIDE_EFFECT;
    arr_val->line = (uint32_t) node->line;

    for (int i = 0; i < count; i++) {
        if (!lower_fixed_array_store_elem(l, node, arr_val, i, elem_vals[i],
                                          target_type->fixed_array.element_type, native))
            return NULL;
    }
    return arr_val;
}

static XrClassInfo *xi_lower_ct_struct_class_info(XiLower *l, struct XrType *target_type,
                                                  const XrCtStructValue *st) {
    if (target_type &&
        (target_type->kind == XR_KIND_INSTANCE || target_type->kind == XR_KIND_CLASS) &&
        target_type->instance.class_ref)
        return target_type->instance.class_ref;
    const char *class_name = target_type ? xr_type_get_class_name(target_type) : NULL;
    if (!class_name && st)
        class_name = st->struct_name;
    return class_name ? xi_lower_lookup_class_info(l, class_name) : NULL;
}

static struct XrType *xi_lower_ct_struct_declared_field_type(XiLower *l, XrClassInfo *info,
                                                             const char *field_name) {
    if (!l || !l->analyzer || !info || !field_name)
        return NULL;
    XaSymbol *field = xa_class_info_lookup_instance_member(info, field_name);
    XaSymbolLinks *links = field ? xa_analyzer_get_links(l->analyzer, field) : NULL;
    return links ? links->type : NULL;
}

static XiValue *lower_ct_struct_value(XiLower *l, AstNode *node, const XrCtValue *value,
                                      struct XrType *target_type) {
    if (!l || !node || !value || value->kind != XR_CT_STRUCT_VALUE)
        return NULL;

    const XrCtStructValue *st = &value->as.struct_val;
    const char *struct_name = st->struct_name;
    struct XrType *result_type = target_type;
    if ((!result_type || XR_TYPE_IS_UNKNOWN(result_type)) && struct_name)
        result_type = xr_type_new_named_instance(l->isolate, struct_name);
    if (!result_type)
        result_type = l->type_any;

    XrAggregateLayout *slayout = xi_lower_type_struct_layout(l, result_type);
    if (!slayout && struct_name)
        slayout = xi_lower_lookup_struct_layout(l, struct_name);
    XrClassInfo *info = xi_lower_ct_struct_class_info(l, result_type, st);

    int count = st->field_count;
    if (count < 0 || count > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] comptime struct value field count exceeds %d at line %d\n",
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }

    int alloc_n = count > 0 ? count : 1;
    XiValue **field_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!field_vals)
        return NULL;

    for (int i = 0; i < count; i++) {
        const char *field_name = st->field_names ? st->field_names[i] : NULL;
        struct XrType *field_type = xi_lower_ct_struct_declared_field_type(l, info, field_name);
        if (slayout) {
            int fidx = xi_lower_struct_field_index(slayout, field_name);
            if (fidx >= 0)
                field_type = xi_lower_struct_field_type(l, field_type, slayout, fidx);
        }
        field_vals[i] = lower_ct_value(l, node, &st->field_values[i], field_type);
        if (!field_vals[i])
            return NULL;
    }

    XiValue *inst = lower_construct(l, node, result_type, NULL, struct_name, NULL, NULL, 0);
    if (!inst)
        return NULL;

    for (int i = 0; i < count; i++) {
        const char *field_name = st->field_names ? st->field_names[i] : NULL;
        if (!field_name || !field_vals[i])
            continue;
        if (slayout) {
            int fidx = xi_lower_struct_field_index(slayout, field_name);
            if (fidx < 0)
                continue;
            XiValue *field_val = xi_lower_narrow_for_native_field(
                l, node, field_vals[i], slayout->fields[fidx].native_type);
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_AGG_SET, l->type_unit, 2);
            if (!set)
                return NULL;
            set->args[0] = inst;
            set->args[1] = field_val;
            set->aux = (void *) slayout;
            set->aux_int = fidx;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) node->line;
            continue;
        }

        XiValue *set = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, l->type_unit, 2);
        if (!set)
            return NULL;
        set->args[0] = inst;
        set->args[1] = field_vals[i];
        set->aux = (void *) arena_strdup(l->func, field_name);
        set->aux_int = xi_lower_method_symbol(l, field_name);
        set->flags |= XI_FLAG_SIDE_EFFECT;
        set->line = (uint32_t) node->line;
    }
    return inst;
}

static XiValue *lower_ct_value(XiLower *l, AstNode *node, const XrCtValue *value,
                               struct XrType *target_type) {
    XiValue *scalar = lower_ct_scalar_value(l, value);
    if (scalar)
        return scalar;
    if (!l || !node || !value)
        return NULL;
    switch (value->kind) {
        case XR_CT_FIXED_ARRAY:
            return lower_ct_fixed_array_value(l, node, value, target_type);
        case XR_CT_TUPLE:
            return lower_ct_tuple_value(l, node, value, target_type);
        case XR_CT_STRUCT_VALUE:
            return lower_ct_struct_value(l, node, value, target_type);
        default:
            return NULL;
    }
}

/* Generate a location string constant for assert diagnostics.
 * Format: "line <N>" using the AST node's line number. */
static const char *make_assert_loc(XiLower *l, int line) {
    char buf[64];
    snprintf(buf, sizeof(buf), "line %d", line);
    return arena_strdup(l->func, buf);
}

/* Intercept known compile-time builtin function calls.
 * Returns non-NULL XiValue if handled, NULL to fall through to generic CALL. */
static XiValue *lower_builtin_call(XiLower *l, AstNode *node, const char *fname,
                                   CallExprNode *call) {
    struct XrType *rtype = xi_lower_node_type(l, node);
    int line = node->line;

    /* assert(cond) / assert(cond, msg) → XI_ASSERT */
    if (strcmp(fname, "assert") == 0 && (call->arg_count == 1 || call->arg_count == 2)) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 0; /* 0 = assert_true */
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_true(cond) → XI_ASSERT aux_int=0 */
    if (strcmp(fname, "assert_true") == 0 && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 0;
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_false(cond) → XI_ASSERT aux_int=1 */
    if (strcmp(fname, "assert_false") == 0 && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 1; /* 1 = assert_false */
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* likely(cond) / unlikely(cond) are semantic identity over bool.
     * AOT consumes the copy kind when this value controls a branch; VM sees
     * an ordinary copy, so functionality stays aligned. */
    if ((strcmp(fname, "likely") == 0 || strcmp(fname, "unlikely") == 0) && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_COPY, l->type_bool, 1);
        if (!v)
            return NULL;
        v->args[0] = cond;
        v->aux_int = (strcmp(fname, "likely") == 0) ? XI_COPY_KIND_LIKELY : XI_COPY_KIND_UNLIKELY;
        v->line = (uint32_t) line;
        return v;
    }
    /* assert_eq(actual, expected) → XI_ASSERT_EQ */
    if (strcmp(fname, "assert_eq") == 0 && call->arg_count == 2) {
        XiValue *actual = xi_lower_expr(l, call->arguments[0]);
        XiValue *expected = xi_lower_expr(l, call->arguments[1]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_EQ, l->type_unit, 2);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = actual;
        v->args[1] = expected;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_ne(actual, unexpected) → XI_ASSERT_NE */
    if (strcmp(fname, "assert_ne") == 0 && call->arg_count == 2) {
        XiValue *actual = xi_lower_expr(l, call->arguments[0]);
        XiValue *unexpected = xi_lower_expr(l, call->arguments[1]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_NE, l->type_unit, 2);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = actual;
        v->args[1] = unexpected;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_throws(fn) → XI_ASSERT_THROWS */
    if (strcmp(fname, "assert_throws") == 0 && call->arg_count == 1) {
        XiValue *fn_val = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_THROWS, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = fn_val;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* typeOf(x) → TypeId int. */
    if (strcmp(fname, "typeOf") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_TYPEID, l->type_int, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        v->line = (uint32_t) line;
        return v;
    }
    /* typeName<T>() → compile-time type display name string. */
    if (strcmp(fname, "typeName") == 0 && call->type_arg_count == 1 && call->arg_count == 0 &&
        call->type_args && call->type_args[0]) {
        XrType *target = l->analyzer ? xr_tref_resolve_in_analyzer(l->analyzer, call->type_args[0])
                                     : xr_tref_resolve(l->isolate, call->type_args[0]);
        return xi_const_str(l->func, l->cur_block, target ? xr_type_to_string(target) : "unknown",
                            l->type_string);
    }
    /* typeName(x) → cold/debug type display name string. */
    if (strcmp(fname, "typeName") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        if (arg && arg->type && arg->type->kind == XR_KIND_SLICE) {
            return xi_const_str(l->func, l->cur_block, TYPE_NAME_SLICE, l->type_string);
        }
        if (arg && arg->type &&
            (arg->type->kind == XR_KIND_FIXED_ARRAY || arg->type->kind == XR_KIND_RUNE ||
             xr_type_is_exact_u8(arg->type))) {
            return xi_const_str(l->func, l->cur_block, xr_type_to_string(arg->type),
                                l->type_string);
        }
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_TYPENAME, l->type_string, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        v->line = (uint32_t) line;
        return v;
    }
    /* len(x) is a compiler-known query, never an ordinary public member call. */
    if (strcmp(fname, "len") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        struct XrType *query_type = xi_lower_node_type(l, call->arguments[0]);
        if (arg && query_type && !xi_lower_type_is_unknown(query_type) &&
            (!arg->type || xi_lower_type_is_unknown(arg->type)))
            arg->type = query_type;
        if (arg && arg->type && arg->type->kind == XR_KIND_FIXED_ARRAY)
            return xi_const_int(l->func, l->cur_block, arg->type->fixed_array.length, l->type_int);

        if (arg && arg->type &&
            (arg->type->kind == XR_KIND_INSTANCE || arg->type->kind == XR_KIND_CLASS)) {
            XrClassInfo *info = arg->type->instance.class_ref;
            if (info && xa_class_info_lookup_member(info, "__operator_len")) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_int, 1);
                if (!v)
                    return xi_const_null(l->func, l->cur_block, l->type_null);
                v->args[0] = arg;
                v->aux = (void *) "__operator_len";
                v->aux_int = (int64_t) xi_lower_method_symbol(l, "__operator_len") << 1;
                v->line = (uint32_t) line;
                return v;
            }
        }

        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LEN, l->type_int, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        v->aux_int = arg && arg->type &&
                             (arg->type->kind == XR_KIND_JSON || arg->type->kind == XR_KIND_UNKNOWN)
                         ? 1
                         : 0;
        if (v->aux_int)
            v->flags |= XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return v;
    }
    /* dump(x) / dump(x, indent) → XI_CALL_BUILTIN aux="dump" → OP_DUMP */
    if (strcmp(fname, "dump") == 0 && (call->arg_count == 1 || call->arg_count == 2)) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        int nargs = (int) call->arg_count;
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, l->type_unit, (uint16_t) nargs);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        if (nargs == 2)
            v->args[1] = xi_lower_expr(l, call->arguments[1]);
        v->aux = (void *) "dump";
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* copy(x) → XI_CALL_BUILTIN aux="copy" → OP_COPY */
    if (strcmp(fname, "copy") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, rtype ? rtype : l->type_any, 1);
        if (!v)
            return NULL;
        v->args[0] = arg;
        v->aux = (void *) "copy";
        v->line = (uint32_t) line;
        return v;
    }
    /* chr(x) → XI_CALL_BUILTIN aux="chr" → OP_CHR */
    if (strcmp(fname, "chr") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, l->type_string, 1);
        if (!v)
            return NULL;
        v->args[0] = arg;
        v->aux = (void *) "chr";
        v->line = (uint32_t) line;
        return v;
    }
    /* print(...) in expression context (e.g. match arm body).
     * Statement-level print is handled by AST_PRINT_STMT → lower_print(),
     * but expression-level calls (AST_CALL_EXPR on variable "print") arrive
     * here.  Emit XI_PRINT instructions with the same encoding. */
    if (strcmp(fname, "print") == 0) {
        int n = (int) call->arg_count;
        XiValue *stack_args[XI_LOWER_VALUE_LIST_STACK_CAP];
        XiLowerValueList args;
        xi_lower_value_list_init(&args, stack_args, XI_LOWER_VALUE_LIST_STACK_CAP);
        for (int i = 0; i < n; i++) {
            XiValue *arg = xi_lower_expr(l, call->arguments[i]);
            if (!arg)
                return xi_const_null(l->func, l->cur_block, l->type_null);
            if (!xi_lower_value_list_push(l, &args, arg, XI_LOWER_MAX_VARIADIC_VALUES,
                                          "print argument count", line))
                return xi_const_null(l->func, l->cur_block, l->type_null);
        }
        for (int i = 0; i < args.count; i++) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT, l->type_unit, 1);
            if (!v)
                return xi_const_null(l->func, l->cur_block, l->type_null);
            v->args[0] = args.items[i];
            int add_space = (i > 0) ? 1 : 0;
            int newline = (i == args.count - 1) ? 1 : 0;
            v->aux_int = add_space | (newline << 1);
            v->flags = xi_op_default_effects(XI_PRINT);
            v->line = (uint32_t) line;
        }
        if (args.count == 0) {
            /* print() with no args → emit newline */
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT, l->type_unit, 1);
            if (!v)
                return xi_const_null(l->func, l->cur_block, l->type_null);
            v->args[0] = xi_const_null(l->func, l->cur_block, l->type_null);
            v->aux_int = (1 << 1) | (1 << 4); /* newline + skip_null */
            v->flags = xi_op_default_effects(XI_PRINT);
            v->line = (uint32_t) line;
        }
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }

    /* Type conversion builtins: string(x), int(x), float(x), bool(x).
     * Each emits XI_CONVERT with the target type set on the value. */
    if (call->arg_count == 1) {
        struct XrType *target = NULL;
        if (strcmp(fname, "string") == 0)
            target = l->type_string;
        else if (strcmp(fname, "int") == 0)
            target = l->type_int;
        else if (strcmp(fname, "float") == 0)
            target = l->type_float;
        else if (strcmp(fname, "bool") == 0)
            target = l->type_bool;
        else if (strcmp(fname, "rune") == 0)
            target = l->type_rune;

        if (target) {
            AstNode *arg_node = call->arguments[0];
            if (arg_node && arg_node->type == AST_SLICE_EXPR && !arg_node->as.slice_expr.start &&
                !arg_node->as.slice_expr.end)
                arg_node = arg_node->as.slice_expr.source;
            XiValue *arg = xi_lower_expr(l, arg_node);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CONVERT, target, 1);
            if (!v)
                return NULL;
            v->args[0] = arg;
            if (arg && arg->type && XR_TYPE_IS_NUMERIC(target) && XR_TYPE_IS_NUMERIC(arg->type)) {
                v->conversion.kind = xr_type_numeric_conversion_kind(target, arg->type);
                v->conversion.source_scalar_rep = arg->type->scalar_rep;
                v->conversion.target_scalar_rep = target->scalar_rep;
                v->conversion.is_implicit = false;
                if (XR_TYPE_IS_FLOAT(arg->type) && XR_TYPE_IS_INT(target))
                    v->flags |= XI_FLAG_MAY_THROW;
            }
            v->line = (uint32_t) line;
            return v;
        }
    }

    (void) rtype;
    return NULL; /* not a builtin — fall through to generic CALL */
}

/* Map Coro.method() names to XI_CORO_OP sub-type constants.
 * Returns -1 for unknown methods. */
static int coro_method_sub_type(const char *method) {
    XR_DCHECK(method != NULL, "coro_method_sub_type: NULL method");
    if (strcmp(method, "Local") == 0)
        return XI_CORO_SUB_LOCAL_NEW;
    /* Dedicated opcodes */
    if (strcmp(method, "lockThread") == 0)
        return XI_CORO_SUB_LOCK_THREAD;
    if (strcmp(method, "unlockThread") == 0)
        return XI_CORO_SUB_UNLOCK_THREAD;
    /* OP_CORO_CTRL sub-opcodes (CORO_CTRL_* values from xchunk.h) */
    if (strcmp(method, "stats") == 0)
        return XI_CORO_SUB_CTRL_BASE + 0;
    if (strcmp(method, "list") == 0)
        return XI_CORO_SUB_CTRL_BASE + 1;
    if (strcmp(method, "dump") == 0)
        return XI_CORO_SUB_CTRL_BASE + 3;
    if (strcmp(method, "stalled") == 0)
        return XI_CORO_SUB_CTRL_BASE + 4;
    if (strcmp(method, "deadlocks") == 0)
        return XI_CORO_SUB_CTRL_BASE + 5;
    if (strcmp(method, "top") == 0)
        return XI_CORO_SUB_CTRL_BASE + 6;
    if (strcmp(method, "groupBy") == 0)
        return XI_CORO_SUB_CTRL_BASE + 7;
    if (strcmp(method, "whereis") == 0)
        return XI_CORO_SUB_CTRL_BASE + 8;
    if (strcmp(method, "monitor") == 0)
        return XI_CORO_SUB_CTRL_BASE + 9;
    if (strcmp(method, "demonitor") == 0)
        return XI_CORO_SUB_CTRL_BASE + 10;
    if (strcmp(method, "self") == 0)
        return XI_CORO_SUB_CTRL_BASE + 11;
    if (strcmp(method, "kill") == 0)
        return XI_CORO_SUB_CTRL_BASE + 12;
    return -1;
}

#define XI_LOWER_CALL_ARG_STACK_CAP 32
#define XI_LOWER_MAX_CALL_ARGS ((int) UINT16_MAX - 1)

typedef struct XiLowerArgList {
    XiValue **items;
    int count;
    int cap;
} XiLowerArgList;

typedef struct XiLowerGoArgList {
    XiValue **items;
    uint8_t *modes;
    int count;
    int cap;
} XiLowerGoArgList;

static void xi_lower_arg_list_init(XiLowerArgList *list, XiValue **stack_items, int stack_cap) {
    list->items = stack_items;
    list->count = 0;
    list->cap = stack_cap;
}

static bool xi_lower_arg_list_grow(XiLower *l, XiLowerArgList *list, int max_args) {
    int next_cap = list->cap > 0 ? list->cap * 2 : 1;
    if (next_cap <= list->count)
        next_cap = list->count + 1;
    if (next_cap > max_args)
        next_cap = max_args;
    if (next_cap <= list->cap)
        return false;

    XiValue **items =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (next_cap * (int) sizeof(XiValue *)));
    if (!items) {
        l->had_error = true;
        return false;
    }
    if (list->count > 0)
        memcpy(items, list->items, (size_t) list->count * sizeof(XiValue *));
    list->items = items;
    list->cap = next_cap;
    return true;
}

static bool xi_lower_arg_list_push(XiLower *l, XiLowerArgList *list, XiValue *value, int max_args,
                                   int line) {
    if (list->count >= max_args) {
        fprintf(stderr, "[LOWER] call argument count exceeds %d at line %d\n", max_args, line);
        l->had_error = true;
        return false;
    }
    if (list->count >= list->cap && !xi_lower_arg_list_grow(l, list, max_args)) {
        l->had_error = true;
        return false;
    }
    list->items[list->count++] = value;
    return true;
}

static void xi_lower_go_arg_list_init(XiLowerGoArgList *list, XiValue **stack_items,
                                      uint8_t *stack_modes, int stack_cap) {
    list->items = stack_items;
    list->modes = stack_modes;
    list->count = 0;
    list->cap = stack_cap;
}

static bool xi_lower_go_arg_list_grow(XiLower *l, XiLowerGoArgList *list, int max_args) {
    int next_cap = list->cap > 0 ? list->cap * 2 : 1;
    if (next_cap <= list->count)
        next_cap = list->count + 1;
    if (next_cap > max_args)
        next_cap = max_args;
    if (next_cap <= list->cap)
        return false;

    XiValue **items =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (next_cap * (int) sizeof(XiValue *)));
    uint8_t *modes =
        (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) (next_cap * (int) sizeof(uint8_t)));
    if (!items || !modes) {
        l->had_error = true;
        return false;
    }
    if (list->count > 0) {
        memcpy(items, list->items, (size_t) list->count * sizeof(XiValue *));
        memcpy(modes, list->modes, (size_t) list->count * sizeof(uint8_t));
    }
    list->items = items;
    list->modes = modes;
    list->cap = next_cap;
    return true;
}

static bool xi_lower_go_arg_list_push(XiLower *l, XiLowerGoArgList *list, XiValue *value,
                                      uint8_t mode, int max_args, int line) {
    if (list->count >= max_args) {
        fprintf(stderr, "[LOWER] go argument count exceeds %d at line %d\n", max_args, line);
        l->had_error = true;
        return false;
    }
    if (list->count >= list->cap && !xi_lower_go_arg_list_grow(l, list, max_args)) {
        l->had_error = true;
        return false;
    }
    list->items[list->count] = value;
    list->modes[list->count] = mode;
    list->count++;
    return true;
}

static bool lower_expr_is_copy_call(AstNode *node, AstNode **inner_out) {
    if (inner_out)
        *inner_out = NULL;
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count != 1 || !call->callee || call->callee->type != AST_VARIABLE)
        return false;
    const char *name = call->callee->as.variable.name;
    if (!name || strcmp(name, "copy") != 0)
        return false;
    if (inner_out)
        *inner_out = call->arguments[0];
    return true;
}

static bool lower_go_call_args(XiLower *l, CallExprNode *call, XiLowerGoArgList *args, int max_args,
                               int line) {
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *child = call->arguments[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return false;
            int arity = src->type ? xr_type_tuple_count(src->type) : 0;
            for (int j = 0; j < arity; j++) {
                struct XrType *et = xr_type_tuple_get(src->type, j);
                XiValue *get =
                    xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et ? et : l->type_any, 1);
                if (!get)
                    return false;
                get->args[0] = src;
                get->aux_int = j;
                if (!xi_lower_go_arg_list_push(l, args, get, XR_TRANSFER_SHARE, max_args, line))
                    return false;
            }
            continue;
        }

        uint8_t mode = XR_TRANSFER_SHARE;
        AstNode *value_node = child;
        if (child->type == AST_MOVE_EXPR) {
            mode = XR_TRANSFER_MOVE;
        } else {
            AstNode *copy_inner = NULL;
            if (lower_expr_is_copy_call(child, &copy_inner) && copy_inner) {
                mode = XR_TRANSFER_COPY;
                value_node = copy_inner;
            }
        }

        XiValue *a = xi_lower_expr(l, value_node);
        if (!a)
            return false;
        if (!xi_lower_go_arg_list_push(l, args, a, mode, max_args, line))
            return false;
    }
    return true;
}

XR_FUNC bool xi_lower_boundary_transfer_arg(XiLower *l, AstNode *child, XiValue **out_value,
                                            uint8_t *out_mode) {
    if (out_value)
        *out_value = NULL;
    if (out_mode)
        *out_mode = XR_TRANSFER_SHARE;
    if (!child || !out_value || !out_mode)
        return false;

    uint8_t mode = XR_TRANSFER_SHARE;
    AstNode *value_node = child;
    if (child->type == AST_MOVE_EXPR) {
        mode = XR_TRANSFER_MOVE;
    } else {
        AstNode *copy_inner = NULL;
        if (lower_expr_is_copy_call(child, &copy_inner) && copy_inner) {
            mode = XR_TRANSFER_COPY;
            value_node = copy_inner;
        }
    }

    XiValue *value = xi_lower_expr(l, value_node);
    if (!value)
        return false;
    *out_value = value;
    *out_mode = mode;
    return true;
}

static bool lower_call_is_sys_thread_spawn(const CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *spawn = &call->callee->as.member_access;
    if (!spawn->name || strcmp(spawn->name, "spawn") != 0 || !spawn->object ||
        spawn->object->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *thread = &spawn->object->as.member_access;
    if (!thread->name || strcmp(thread->name, "Thread") != 0 || !thread->object ||
        thread->object->type != AST_VARIABLE)
        return false;
    const char *module_name = thread->object->as.variable.name;
    return module_name && strcmp(module_name, "sys") == 0;
}

static AstNode *lower_thread_spawn_body_arg(CallExprNode *call) {
    if (!call)
        return NULL;
    if (call->arg_count == 1)
        return call->arguments[0];
    if (call->arg_count == 2)
        return call->arguments[1];
    return NULL;
}

static XiThreadSpawnOptions lower_thread_spawn_options(CallExprNode *call) {
    XiThreadSpawnOptions opts = {0};
    if (!call || call->arg_count != 2 || !call->arguments[0] ||
        call->arguments[0]->type != AST_STRUCT_LITERAL)
        return opts;
    StructLiteralNode *sl = &call->arguments[0]->as.struct_literal;
    if (!sl->struct_name || strcmp(sl->struct_name, "ThreadOptions") != 0)
        return opts;
    for (int i = 0; i < sl->field_count; i++) {
        const char *name = sl->field_names ? sl->field_names[i] : NULL;
        AstNode *value = sl->field_values ? sl->field_values[i] : NULL;
        if (!name || !value)
            continue;
        if (strcmp(name, "stackSize") == 0 && value->type == AST_LITERAL_INT) {
            int64_t n = value->as.literal.raw_value.int_val;
            opts.stack_size = n > 0 ? n : 0;
        } else if (strcmp(name, "name") == 0 && value->type == AST_LITERAL_STRING) {
            opts.name = value->as.literal.raw_value.string_val;
        } else if (strcmp(name, "affinity") == 0 && value->type == AST_ARRAY_LITERAL) {
            ArrayLiteralNode *arr = &value->as.array_literal;
            int count = arr->count < XR_THREAD_AFFINITY_MAX ? arr->count : XR_THREAD_AFFINITY_MAX;
            for (int ai = 0; ai < count; ai++) {
                AstNode *elem = arr->elements ? arr->elements[ai] : NULL;
                if (!elem || elem->type != AST_LITERAL_INT)
                    continue;
                int64_t cpu = elem->as.literal.raw_value.int_val;
                if (cpu < 0)
                    continue;
                opts.affinity_cpus[opts.affinity_count++] = (uint32_t) cpu;
            }
        }
    }
    return opts;
}

static bool lower_thread_spawn_attach_options(XiLower *l, XiValue *v,
                                              const XiThreadSpawnOptions *opts,
                                              const uint8_t *modes, int mode_count) {
    if (!l || !v)
        return false;
    bool has_modes = modes && mode_count > 0;
    bool has_options = opts && (opts->name || opts->stack_size > 0 || opts->affinity_count > 0);
    if (!has_modes && !has_options)
        return true;

    XiThreadSpawnOptions *payload =
        (XiThreadSpawnOptions *) xi_func_arena_alloc(l->func, sizeof(*payload));
    if (!payload)
        return false;
    memset(payload, 0, sizeof(*payload));
    if (opts) {
        payload->stack_size = opts->stack_size;
        payload->name = opts->name ? arena_strdup(l->func, opts->name) : NULL;
        if (opts->name && !payload->name)
            return false;
        payload->affinity_count = opts->affinity_count;
        memcpy(payload->affinity_cpus, opts->affinity_cpus,
               (size_t) opts->affinity_count * sizeof(uint32_t));
    }
    if (has_modes) {
        payload->transfer_modes = (uint8_t *) xi_func_arena_alloc(
            l->func, (uint32_t) ((size_t) mode_count * sizeof(uint8_t)));
        if (!payload->transfer_modes)
            return false;
        memcpy(payload->transfer_modes, modes, (size_t) mode_count * sizeof(uint8_t));
    }
    v->aux = payload;
    v->aux_kind = XI_AUX_KIND_THREAD_SPAWN;
    v->aux_int = pack_thread_spawn_aux(payload->stack_size);
    return true;
}

static XiValue *lower_thread_spawn_expr(XiLower *l, AstNode *node, AstNode *expr,
                                        const XiThreadSpawnOptions *opts) {
    struct XrType *result_type = xi_lower_node_type(l, node);
    if (!expr)
        return NULL;

    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee)
            return NULL;
        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        uint8_t stack_modes[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerGoArgList args;
        xi_lower_go_arg_list_init(&args, stack_args, stack_modes, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_go_call_args(l, call, &args, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
        int n = args.count;
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_THREAD_SPAWN, result_type, (uint16_t) (1 + n));
        if (!v)
            return NULL;
        v->args[0] = callee;
        for (int i = 0; i < n; i++)
            v->args[1 + i] = args.items[i];
        v->aux_int = pack_thread_spawn_aux(opts ? opts->stack_size : 0);
        if (!lower_thread_spawn_attach_options(l, v, opts, args.modes, n))
            return NULL;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *callee = xi_lower_expr(l, expr);
    if (!callee)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_THREAD_SPAWN, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = callee;
    v->aux_int = pack_thread_spawn_aux(opts ? opts->stack_size : 0);
    if (!lower_thread_spawn_attach_options(l, v, opts, NULL, 0))
        return NULL;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_thread_spawn_call(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!call)
        return NULL;
    XiThreadSpawnOptions opts = lower_thread_spawn_options(call);
    return lower_thread_spawn_expr(l, node, lower_thread_spawn_body_arg(call), &opts);
}

/* Lower Coro.method(args...) → XI_CORO_OP.
 * Returns NULL for unrecognized methods. */
static XiValue *lower_coro_method(XiLower *l, AstNode *node, const char *method,
                                  CallExprNode *call) {
    /* Coro.yield(): cooperative CPU yield (Gosched). Lowers to an immediate
     * XI_YIELD suspend point — the same primitive the former bare `yield`
     * statement used. `yield expr` is reserved for generator value production. */
    if (strcmp(method, "yield") == 0) {
        if (call->arg_count != 0) {
            fprintf(stderr, "[LOWER] Coro.yield() takes no arguments at line %d\n",
                    (int) node->line);
            l->had_error = true;
            return NULL;
        }
        XiValue *y = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_unit, 0);
        if (!y)
            return NULL;
        y->aux_int = XI_YIELD_AUX_IMMEDIATE;
        y->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
        y->line = (uint32_t) node->line;
        return y;
    }
    int sub = coro_method_sub_type(method);
    if (sub < 0)
        return NULL;

    int n = call->arg_count;
    if (n < 0 || n > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] Coro.%s argument count exceeds %d at line %d\n", method,
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }
    XiValue *stack_args[16];
    XiValue **arg_vals = stack_args;
    if (n > 16) {
        arg_vals =
            (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) n * sizeof(XiValue *)));
        if (!arg_vals)
            return NULL;
    }
    for (int i = 0; i < n; i++) {
        arg_vals[i] = xi_lower_expr(l, call->arguments[i]);
        if (!arg_vals[i])
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CORO_OP, result_type, (uint16_t) n);
    if (!v)
        return NULL;
    for (int i = 0; i < n; i++)
        v->args[i] = arg_vals[i];
    v->aux_int = sub;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_coro_pool_submit(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!l || !node || !call || call->arg_count != 1 || !call->arguments || !call->arguments[0])
        return NULL;
    XiValue *callee = xi_lower_expr(l, call->arguments[0]);
    if (!callee)
        return NULL;
    XiValue *task = xi_value_new(l->func, l->cur_block, XI_GO, xi_lower_node_type(l, node), 1);
    if (!task)
        return NULL;
    task->args[0] = callee;
    task->aux_int = (int64_t) pack_go_aux(0);
    task->flags |= XI_FLAG_SIDE_EFFECT;
    task->line = (uint32_t) node->line;
    return task;
}

/* Lower the argument list of a call, expanding any AST_SPREAD_EXPR
 * `...t` into one TUPLE_GET per static element of the source tuple.
 * Returns the effective argument count written into `args`. The
 * caller-supplied `pmodes`/`pcount` apply XR_PARAM_READ deep-copy
 * semantics to value-type slots; spread-expanded slots are not copied
 * (the source tuple already owns the element). */
static bool lower_call_args_expand_spread(XiLower *l, CallExprNode *call, XiLowerArgList *args,
                                          int max_args, const XrParamMode *pmodes, int pcount,
                                          const bool *read_places, int read_place_count, int line) {
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *child = call->arguments[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return false;
            int arity = src->type ? xr_type_tuple_count(src->type) : 0;
            for (int j = 0; j < arity; j++) {
                struct XrType *et = xr_type_tuple_get(src->type, j);
                XiValue *get =
                    xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et ? et : l->type_any, 1);
                if (!get)
                    return false;
                get->args[0] = src;
                get->aux_int = j;
                if (!xi_lower_arg_list_push(l, args, get, max_args, line))
                    return false;
            }
            continue;
        }

        XiValue *a = xi_lower_expr(l, child);
        if (!a)
            return false;
        XrParamMode mode = (pmodes && args->count < pcount) ? pmodes[args->count] : XR_PARAM_READ;
        bool read_place = read_places && args->count < read_place_count && read_places[args->count];
        if (xi_lower_value_needs_value_clone(l, a) && !xi_lower_value_is_fresh_value_struct(a) &&
            mode == XR_PARAM_READ && !read_place) {
            XiValue *cpy = xi_value_new(l->func, l->cur_block, XI_COPY, a->type, 1);
            if (cpy) {
                cpy->args[0] = a;
                xi_lower_mark_value_clone_copy(cpy);
                a = cpy;
            }
        }
        if (!xi_lower_arg_list_push(l, args, a, max_args, line))
            return false;
    }
    return true;
}

typedef struct XiCallWriteback {
    XiValue *source;
    XiValue *place;
    XiTopBinding top_binding;
    int var_id;
    bool projection;
} XiCallWriteback;

static AstNode *lower_call_unwrap_place(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node;
}

static XrCallArgAccess lower_call_arg_access(const CallExprNode *call, int index) {
    if (!call || index < 0 || index >= call->arg_count || !call->arg_accesses)
        return XR_CALL_ARG_PLAIN;
    XrCallArgAccess access = call->arg_accesses[index];
    return xr_call_arg_access_is_valid(access) ? access : XR_CALL_ARG_PLAIN;
}

static XiValue *lower_call_projection_base(XiValue *value) {
    while (value && value->nargs >= 1) {
        switch (value->op) {
            case XI_COPY:
            case XI_SOURCE_MOVE:
            case XI_OWNER_FORWARD:
            case XI_NARROW_I8:
            case XI_NARROW_U8:
            case XI_NARROW_I16:
            case XI_NARROW_U16:
            case XI_NARROW_I32:
            case XI_NARROW_U32:
            case XI_NARROW_F32:
            case XI_WIDEN_I8:
            case XI_WIDEN_U8:
            case XI_WIDEN_I16:
            case XI_WIDEN_U16:
            case XI_WIDEN_I32:
            case XI_WIDEN_U32:
            case XI_WIDEN_F32:
                value = value->args[0];
                continue;
            default:
                return value;
        }
    }
    return value;
}

static bool lower_call_store_projection(XiLower *l, XiValue *source, XiValue *updated, int line) {
    XiValue *base = lower_call_projection_base(source);
    if (!l || !base || !updated)
        return false;
    XiValue *store = NULL;
    switch (base->op) {
        case XI_LOAD_FIELD:
            store = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, l->type_unit, 2);
            if (store) {
                store->args[0] = base->args[0];
                store->args[1] = updated;
                store->aux = base->aux;
                store->aux_int = base->aux_int;
                store->xg_class_field_id = base->xg_class_field_id;
            }
            break;
        case XI_AGG_GET:
            store = xi_value_new(l->func, l->cur_block, XI_AGG_SET, l->type_unit, 2);
            if (store) {
                store->args[0] = base->args[0];
                store->args[1] = updated;
                store->aux = base->aux;
                store->aux_int = base->aux_int;
            }
            break;
        case XI_INDEX_GET:
            store = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
            if (store) {
                store->args[0] = base->args[0];
                store->args[1] = base->args[1];
                store->args[2] = updated;
                store->xg_sequence_access_id = base->xg_sequence_access_id;
                store->xg_key_access_id = base->xg_key_access_id;
            }
            break;
        case XI_JSON_GET_F:
            store = xi_value_new(l->func, l->cur_block, XI_JSON_SET_F, l->type_unit, 2);
            if (store) {
                store->args[0] = base->args[0];
                store->args[1] = updated;
                store->aux_int = base->aux_int;
                store->xg_json_access_id = base->xg_json_access_id;
            }
            break;
        case XI_PTR_LOAD:
            store = xi_value_new(l->func, l->cur_block, XI_PTR_STORE, l->type_unit, 3);
            if (store) {
                store->args[0] = base->args[0];
                store->args[1] = updated;
                store->args[2] = base->args[1];
                store->aux_int = base->aux_int;
            }
            break;
    }
    if (!store) {
        fprintf(stderr, "[LOWER] unsupported ref projection writeback at line %d\n", line);
        l->had_error = true;
        return false;
    }
    store->line = (uint32_t) line;
    return true;
}

static bool lower_call_slot_uses_read_place(XiLower *l, XrParamMode mode, int index,
                                            const bool *read_places, int read_place_count,
                                            const XrType *function_type, const XiValue *actual) {
    if (mode != XR_PARAM_READ || index < 0)
        return false;
    bool requested = read_places && index < read_place_count && read_places[index];
    if (!requested && function_type && function_type->kind == XR_KIND_FUNCTION &&
        index < function_type->function.param_count) {
        XrType *formal = xr_type_function_param_type(function_type, index);
        requested = xi_lower_type_uses_read_place(l, formal);
    }
    /* A call-place ABI is valid only when both sides agree on a fixed-layout
     * value aggregate.  This guards method signatures whose internal receiver
     * slot is not part of the source argument list. */
    if (!requested || !actual)
        return false;
    return xi_lower_type_uses_read_place(l, actual->type);
}

static XiCallPlan *lower_build_call_plan(XiLower *l, CallExprNode *call, XiValue **arg_vals, int n,
                                         const XrParamMode *pmodes, int pcount,
                                         const bool *read_places, int read_place_count,
                                         const XrType *function_type,
                                         XiCallWriteback **out_writebacks, int line) {
    if (out_writebacks)
        *out_writebacks = NULL;
    bool needs_plan = false;
    for (int i = 0; i < n && i < pcount; i++) {
        XrParamMode mode = pmodes ? pmodes[i] : XR_PARAM_READ;
        if (mode != XR_PARAM_READ ||
            lower_call_slot_uses_read_place(l, mode, i, read_places, read_place_count,
                                            function_type, arg_vals ? arg_vals[i] : NULL)) {
            needs_plan = true;
            break;
        }
    }
    if (!needs_plan)
        return NULL;
    if (!call || n != call->arg_count) {
        fprintf(stderr, "[LOWER] borrowed call arguments cannot be spread at line %d\n", line);
        l->had_error = true;
        return NULL;
    }

    XiCallPlan *plan = (XiCallPlan *) xi_func_arena_alloc(l->func, sizeof(*plan));
    XiCallArgPlan *plans = (XiCallArgPlan *) xi_func_arena_alloc(
        l->func, (uint32_t) ((size_t) n * sizeof(XiCallArgPlan)));
    XiCallWriteback *writebacks = (XiCallWriteback *) xi_func_arena_alloc(
        l->func, (uint32_t) ((size_t) n * sizeof(XiCallWriteback)));
    if (!plan || !plans || !writebacks) {
        l->had_error = true;
        return NULL;
    }
    memset(plan, 0, sizeof(*plan));
    memset(plans, 0, (size_t) n * sizeof(*plans));
    memset(writebacks, 0, (size_t) n * sizeof(*writebacks));
    plan->args = plans;
    plan->nargs = (uint16_t) n;

    for (int i = 0; i < n; i++) {
        XrParamMode mode = (pmodes && i < pcount) ? pmodes[i] : XR_PARAM_READ;
        XrCallArgAccess access = lower_call_arg_access(call, i);
        XiCallArgPlan *arg_plan = &plans[i];
        arg_plan->param_mode = mode;
        arg_plan->access = access;
        arg_plan->origin_var_id = XI_NO_VAR_ID;
        bool read_place =
            lower_call_slot_uses_read_place(l, mode, i, read_places, read_place_count,
                                            function_type, arg_vals ? arg_vals[i] : NULL);
        if (mode == XR_PARAM_READ && !read_place)
            continue;

        bool access_ok =
            (mode == XR_PARAM_READ && access == XR_CALL_ARG_PLAIN) ||
            (mode == XR_PARAM_REF && access == XR_CALL_ARG_REF) ||
            (mode == XR_PARAM_MOVE && (access == XR_CALL_ARG_MOVE || access == XR_CALL_ARG_PLAIN));
        if (!access_ok) {
            fprintf(stderr,
                    "[LOWER] call contract drift at line %d: slot %d is %s but access is %s\n",
                    line, i + 1, xr_param_mode_label(mode), xr_call_arg_access_label(access));
            l->had_error = true;
            return NULL;
        }

        /* A consuming parameter receives an owned value, never an addressable
         * copy-out slot.  `move source` is already represented by XI_OWNER_FORWARD;
         * fresh values and copy(...) arrive as ordinary value expressions. */
        if (mode == XR_PARAM_MOVE)
            continue;

        AstNode *place_node = lower_call_unwrap_place(call->arguments[i]);
        int var_id = -1;
        XiTopBinding top_binding = {.slot = -1, .name = NULL, .type = NULL};
        if (place_node && place_node->type == AST_VARIABLE) {
            var_id = xi_lower_var_find(l, place_node->as.variable.symbol_id,
                                       place_node->as.variable.name);
            if (var_id >= 0 && l->is_program && l->shared_map[var_id] >= 0) {
                top_binding.slot = l->shared_map[var_id];
                top_binding.name = l->vars[var_id].name;
                top_binding.type = l->vars[var_id].type;
            } else if (var_id < 0) {
                top_binding = xi_lower_find_top_binding(l, place_node->as.variable.symbol_id,
                                                        place_node->as.variable.name);
            }
        }

        XiValue *place = NULL;
        if (var_id >= 0 && l->vars[var_id].call_place) {
            place = l->vars[var_id].call_place;
            arg_plan->origin =
                place->op == XI_LOCAL_ADDR ? XI_PLACE_ORIGIN_STACK_LOCAL : XI_PLACE_ORIGIN_PARAM;
            arg_plan->origin_var_id = (XiVarId) var_id;
        } else {
            XiValue *source = arg_vals[i];
            XrType *place_type = source ? source->type : l->type_any;
            if (function_type && function_type->kind == XR_KIND_FUNCTION &&
                i < function_type->function.param_count) {
                XrType *formal = xr_type_function_param_type(function_type, i);
                if (formal)
                    place_type = formal;
            }
            place = xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR, place_type, 1);
            if (!place)
                return NULL;
            place->args[0] = source;
            place->line = (uint32_t) line;
            arg_plan->origin =
                var_id >= 0 ? XI_PLACE_ORIGIN_STACK_LOCAL : XI_PLACE_ORIGIN_PROJECTION_TEMP;
            if (var_id >= 0)
                arg_plan->origin_var_id = (XiVarId) var_id;
            else if (!xi_top_binding_valid(top_binding))
                place->aux_int |= XI_LOCAL_ADDR_AUX_DIRECT_PROJECTION;
            if (mode == XR_PARAM_REF) {
                /* `ref owner[a:b]` borrows the projected element range.  The
                 * analyzer only authorizes it when the callee's canonical
                 * memory summary proves that the Slice descriptor is not
                 * rebound, so no projection writeback exists or is needed. */
                if (!source || source->op != XI_SLICE) {
                    writebacks[i].source = source;
                    writebacks[i].place = place;
                    writebacks[i].top_binding = top_binding;
                    writebacks[i].var_id = var_id;
                    writebacks[i].projection = var_id < 0 && !xi_top_binding_valid(top_binding);
                }
            }
        }
        arg_vals[i] = place;
        arg_plan->place = place;
        arg_plan->addressable = true;
        arg_plan->lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
        arg_plan->escape = XI_PLACE_ESCAPE_NONE;
    }
    plan->verified = true;
    if (out_writebacks)
        *out_writebacks = writebacks;
    return plan;
}

static bool lower_method_receiver_mode(XiLower *l, const XaSelection *selection, XiValue *receiver,
                                       XrParamMode *out_mode) {
    if (!l || !selection || !selection->target_symbol || !receiver || !out_mode ||
        selection->target_symbol->kind != XA_SYM_METHOD || selection->target_symbol->is_static)
        return false;
    /* Selective imports from an embedded stdlib module can carry a nominal
     * receiver view whose value-type bit is not the same allocation as the
     * declaration-owned type.  The selected method's owner is the canonical
     * declaration identity, so its fixed aggregate layout is the authoritative
     * proof that the receiver uses the call-bound-place ABI. */
    bool value_receiver = xi_lower_type_needs_value_clone(l, receiver->type);
    XaSymbol *owner = selection->target_symbol->parent;
    XaSymbolLinks *owner_links = owner ? xa_analyzer_get_links(l->analyzer, owner) : NULL;
    if (owner_links && ((owner_links->type && owner_links->type->is_value_type) ||
                        (owner_links->class_info && owner_links->class_info->struct_layout)))
        value_receiver = true;
    if (!value_receiver)
        return false;
    *out_mode = selection->target_symbol->mutates_receiver ? XR_PARAM_REF : XR_PARAM_READ;
    return true;
}

static XiValue *lower_build_method_receiver_place(XiLower *l, CallExprNode *call,
                                                  AstNode *receiver_node, XiValue *receiver,
                                                  int explicit_arg_count, XrParamMode mode,
                                                  XiCallPlan **plan_io, XiCallWriteback *writeback,
                                                  int line) {
    if (!l || !receiver || !plan_io || !writeback ||
        (mode != XR_PARAM_READ && mode != XR_PARAM_REF))
        return NULL;
    memset(writeback, 0, sizeof(*writeback));
    writeback->var_id = -1;
    writeback->top_binding = (XiTopBinding) {.slot = -1, .name = NULL, .type = NULL};

    AstNode *place_node = lower_call_unwrap_place(receiver_node);
    int var_id = -1;
    XiTopBinding top_binding = {.slot = -1, .name = NULL, .type = NULL};
    if (place_node && place_node->type == AST_VARIABLE) {
        var_id =
            xi_lower_var_find(l, place_node->as.variable.symbol_id, place_node->as.variable.name);
        if (var_id >= 0 && l->is_program && l->shared_map[var_id] >= 0) {
            top_binding.slot = l->shared_map[var_id];
            top_binding.name = l->vars[var_id].name;
            top_binding.type = l->vars[var_id].type;
        } else if (var_id < 0) {
            top_binding = xi_lower_find_top_binding(l, place_node->as.variable.symbol_id,
                                                    place_node->as.variable.name);
        }
    } else if (place_node && place_node->type == AST_THIS_EXPR) {
        var_id = xi_lower_var_find(l, 0, "this");
    }

    XiValue *place = NULL;
    XiPlaceOrigin origin = XI_PLACE_ORIGIN_PROJECTION_TEMP;
    XiVarId origin_var_id = XI_NO_VAR_ID;
    if (var_id >= 0 && l->vars[var_id].call_place) {
        place = l->vars[var_id].call_place;
        origin = place->op == XI_LOCAL_ADDR ? XI_PLACE_ORIGIN_STACK_LOCAL : XI_PLACE_ORIGIN_PARAM;
        origin_var_id = (XiVarId) var_id;
    } else if (receiver->op == XI_PTR_LOAD && receiver->nargs >= 1 && receiver->args[0] &&
               receiver->args[0]->type && XR_TYPE_IS_POINTER(receiver->args[0]->type) &&
               (mode == XR_PARAM_READ || receiver->args[0]->type->ptr_is_mut)) {
        /* Preserve the PTR_LOAD as the semantic source so the VM retains its
         * value/writeback behavior. Native backends may recognize this exact
         * call-bound shape and borrow the pointee address directly. */
        place = xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR, receiver->type, 1);
        if (!place)
            return NULL;
        place->args[0] = receiver;
        place->aux_int = XI_LOCAL_ADDR_AUX_RAW_DEREF;
        place->line = (uint32_t) line;
        if (mode == XR_PARAM_REF) {
            writeback->source = receiver;
            writeback->place = place;
            writeback->top_binding = top_binding;
            writeback->var_id = var_id;
            writeback->projection = var_id < 0 && !xi_top_binding_valid(top_binding);
        }
    } else {
        place = xi_value_new(l->func, l->cur_block, XI_LOCAL_ADDR, receiver->type, 1);
        if (!place)
            return NULL;
        place->args[0] = receiver;
        place->line = (uint32_t) line;
        if (var_id >= 0) {
            origin = XI_PLACE_ORIGIN_STACK_LOCAL;
            origin_var_id = (XiVarId) var_id;
        } else if (!xi_top_binding_valid(top_binding))
            place->aux_int |= XI_LOCAL_ADDR_AUX_DIRECT_PROJECTION;
        if (mode == XR_PARAM_REF) {
            writeback->source = receiver;
            writeback->place = place;
            writeback->top_binding = top_binding;
            writeback->var_id = var_id;
            writeback->projection = var_id < 0 && !xi_top_binding_valid(top_binding);
        }
    }

    XiCallPlan *plan = *plan_io;
    if (!plan) {
        plan = (XiCallPlan *) xi_func_arena_alloc(l->func, sizeof(*plan));
        if (!plan)
            return NULL;
        memset(plan, 0, sizeof(*plan));
        plan->nargs = (uint16_t) explicit_arg_count;
        if (explicit_arg_count > 0) {
            plan->args = (XiCallArgPlan *) xi_func_arena_alloc(
                l->func, (uint32_t) ((size_t) explicit_arg_count * sizeof(*plan->args)));
            if (!plan->args)
                return NULL;
            memset(plan->args, 0, (size_t) explicit_arg_count * sizeof(*plan->args));
            for (int i = 0; i < explicit_arg_count; i++) {
                plan->args[i].param_mode = XR_PARAM_READ;
                plan->args[i].access = lower_call_arg_access(call, i);
                plan->args[i].origin_var_id = XI_NO_VAR_ID;
            }
        }
        plan->verified = true;
        *plan_io = plan;
    }
    plan->has_receiver = true;
    plan->receiver.param_mode = mode;
    plan->receiver.access = XR_CALL_ARG_PLAIN;
    plan->receiver.origin = (uint8_t) origin;
    plan->receiver.lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    plan->receiver.escape = XI_PLACE_ESCAPE_NONE;
    plan->receiver.addressable = true;
    plan->receiver.origin_var_id = origin_var_id;
    plan->receiver.place = place;
    return place;
}

static bool lower_apply_one_call_writeback(XiLower *l, XiCallWriteback *wb, int line) {
    if (!wb || !wb->place)
        return true;
    XiValue *load = xi_value_new(l->func, l->cur_block, XI_PLACE_LOAD,
                                 wb->source ? wb->source->type : l->type_any, 1);
    if (!load)
        return false;
    load->args[0] = wb->place;
    load->line = (uint32_t) line;
    if (xi_top_binding_valid(wb->top_binding)) {
        if (!xi_lower_emit_top_store(l, wb->top_binding, load))
            return false;
        if (wb->var_id >= 0)
            xi_lower_braun_write(l, wb->var_id, l->cur_block, load);
    } else if (wb->var_id >= 0) {
        load->var_id = (XiVarId) wb->var_id;
        xi_lower_braun_write(l, wb->var_id, l->cur_block, load);
    } else if (wb->projection && !lower_call_store_projection(l, wb->source, load, line)) {
        return false;
    }
    return true;
}

static bool lower_apply_call_writebacks(XiLower *l, const XiCallPlan *plan,
                                        XiCallWriteback *writebacks, int line) {
    if (!plan || !writebacks)
        return true;
    for (uint16_t i = 0; i < plan->nargs; i++) {
        XiCallWriteback *wb = &writebacks[i];
        if (!lower_apply_one_call_writeback(l, wb, line))
            return false;
    }
    return true;
}

/* Task 216 P1: whether a call's callee may raise into the error channel.
 * Fail-closed — report false (nothrow) only when the callee is provably
 * NO_THROW, so an error check is elided only where it could never fire. For a
 * direct named call we consult the resolved function symbol's published bit;
 * otherwise we fall back to the static callee function type (which defaults to
 * the fail-closed MAY_THROW). */
static bool lower_call_callee_may_throw(XiLower *l, CallExprNode *call,
                                        struct XrType *callee_type) {
    struct XrType *fn_type = NULL;
    if (l && l->analyzer && call && call->callee && call->callee->type == AST_VARIABLE) {
        XaSymbol *sym =
            xa_scope_lookup_by_id(l->analyzer->global_scope, call->callee->as.variable.symbol_id);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(l->analyzer, sym) : NULL;
        if (links && links->type && links->type->kind == XR_KIND_FUNCTION)
            fn_type = links->type;
    }
    /* A selected value-type method has one closed implementation. Its bound
     * member-expression type is created before the error-set fixed point and
     * can therefore still carry the conservative POLY bit; consult the
     * authoritative selected method symbol instead. Keep open class/interface
     * dispatch fail-closed because an override may have a wider error set. */
    if (!fn_type && l && l->analyzer && call && call->callee &&
        call->callee->type == AST_MEMBER_ACCESS) {
        const XaSelection *sel = xa_analyzer_get_selection(l->analyzer, call->callee);
        XaSymbol *target = sel ? sel->target_symbol : NULL;
        XrType *receiver = sel ? sel->receiver_type : NULL;
        bool closed_value_dispatch =
            target &&
            (target->is_static || (receiver && receiver->kind == XR_KIND_ENUM) ||
             (receiver && receiver->kind == XR_KIND_INSTANCE && receiver->instance.class_ref &&
              receiver->instance.class_ref->struct_layout));
        XaSymbolLinks *links =
            closed_value_dispatch ? xa_analyzer_get_links(l->analyzer, target) : NULL;
        if (links && links->type && links->type->kind == XR_KIND_FUNCTION)
            fn_type = links->type;
    }
    if (!fn_type && callee_type && callee_type->kind == XR_KIND_FUNCTION)
        fn_type = callee_type;
    return !(fn_type && fn_type->function.throw_effect == XR_FN_EFFECT_NO_THROW);
}

/* Apply the constructive error-check decision to a freshly emitted call value:
 * when the callee is proven NO_THROW no XI_ERR_CHECK node is generated at all
 * (the check could never fire). The call value keeps its conservative
 * XI_FLAG_MAY_THROW so optimizer analyses and the existing backend proven-nothrow
 * recovery stay well-defined; only the error-channel check node is elided. */
static void lower_call_emit_err_check(XiLower *l, XiValue *call_v, AstNode *node,
                                      CallExprNode *call, struct XrType *callee_type) {
    (void) call_v;
    xi_lower_insert_err_check(l, node, lower_call_callee_may_throw(l, call, callee_type));
}

static XiValue *lower_emit_function_call(XiLower *l, AstNode *node, CallExprNode *call,
                                         XiValue *callee_val, struct XrType *callee_type) {
    if (!callee_val)
        return NULL;
    callee_type = xr_type_non_nullable(l->isolate, callee_type);
    struct XrType *constructor_type = xi_lower_call_constructor_type(l, call);
    if (!constructor_type)
        constructor_type = xi_lower_type_constructor_type(l, callee_type);
    if (constructor_type)
        callee_type = constructor_type;

    XrParamMode stack_sig_modes[64];
    const XrParamMode *pmodes = NULL;
    int pcount = 0;
    if (callee_type && callee_type->kind == XR_KIND_FUNCTION) {
        pmodes = lower_function_param_modes(
            l, callee_type, stack_sig_modes,
            (int) (sizeof(stack_sig_modes) / sizeof(stack_sig_modes[0])), &pcount);
    }

    XiFunc *static_callee = lower_resolve_static_callee_func(l, callee_val);
    XrParamMode *owned_static_modes = NULL;
    if ((!pmodes || pcount == 0) && static_callee && static_callee->nparams > 0) {
        owned_static_modes = (XrParamMode *) xi_func_arena_alloc(
            l->func, (uint32_t) ((size_t) static_callee->nparams * sizeof(XrParamMode)));
        if (!owned_static_modes)
            return NULL;
        for (uint16_t i = 0; i < static_callee->nparams; i++)
            owned_static_modes[i] = xi_func_param_passing_mode(static_callee, i);
        pmodes = owned_static_modes;
        pcount = static_callee->nparams;
    }

    bool stack_read_places[64];
    bool *read_places = NULL;
    int read_place_count = pcount > 0 ? pcount : (static_callee ? (int) static_callee->nparams : 0);
    if (read_place_count > 0) {
        read_places =
            read_place_count <= (int) (sizeof(stack_read_places) / sizeof(stack_read_places[0]))
                ? stack_read_places
                : (bool *) xi_func_arena_alloc(
                      l->func, (uint32_t) ((size_t) read_place_count * sizeof(bool)));
        if (!read_places)
            return NULL;
        lower_collect_read_place_params(l, static_callee, callee_type, pmodes, pcount, read_places,
                                        read_place_count);
    }

    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, pmodes, pcount,
                                       read_places, read_place_count, (int) node->line))
        return NULL;
    XiValue **arg_vals = args.items;
    int n = args.count;

    /* Which math registry member this call resolves to, if any.  Both spellings
     * must be recognised -- `math.min(...)` and `import { min } from "math"` --
     * because the int-preserving domain below applies to the call, not to the
     * spelling.  The analyzer's symbol record is the primary source; the Xi
     * import ref only covers callees whose binding is visible in this lowerer. */
    const char *math_callee_member = lower_math_member_from_callee_symbol(l, call);
    if (!math_callee_member) {
        const XiImportRef *callee_import = lower_import_ref_from_value(l, callee_val);
        if (callee_import && callee_import->module_path && callee_import->member_name &&
            strcmp(callee_import->module_path, "math") == 0)
            math_callee_member = callee_import->member_name;
    }
    bool math_preserves_int_args =
        lower_math_call_preserves_int_args(math_callee_member, arg_vals, n);

    if (callee_type && callee_type->kind == XR_KIND_FUNCTION) {
        int pc = callee_type->function.param_count;
        for (int i = 0; i < n && i < pc; i++) {
            struct XrType *pt = xr_type_function_param_type(callee_type, i);
            if (!pt || !arg_vals[i] || !arg_vals[i]->type)
                continue;
            /* The public math registry uses a float-shaped callable signature,
             * while abs/min/max/clamp deliberately preserve an all-int call.
             * Analyzer inference has already selected that effective domain.
             * Mirror it for imported-member aliases so lowering does not
             * manufacture an int->float boundary absent from the typed
             * program's conversion snapshot. */
            if (math_preserves_int_args)
                pt = l->type_int;
            if (pt->kind == XR_KIND_TYPE_PARAM && call->type_arg_count > 0 &&
                callee_type->function.type_param_names) {
                const char *tp_name = pt->type_param.name;
                for (int ti = 0; ti < callee_type->function.type_param_count; ti++) {
                    if (callee_type->function.type_param_names[ti] && tp_name &&
                        strcmp(callee_type->function.type_param_names[ti], tp_name) == 0 &&
                        ti < call->type_arg_count && call->type_args[ti]) {
                        pt = xi_lower_type_or_any(l,
                                                  xr_tref_resolve(l->isolate, call->type_args[ti]),
                                                  "call type argument", node ? node->line : 0);
                        break;
                    }
                }
            }
            /* Strict dynamic→concrete argument boundary: a Json/dynamic value passed
             * into a concrete parameter is verified at runtime via OP_CHECKTYPE,
             * exactly like the var-binding / return / map-key boundaries. This keeps
             * VM and AOT raising the same TypeError on mismatch instead of silently
             * coercing (e.g. Json int 1 into a `bool` parameter). */
            if (xr_is_json_coercion(pt, arg_vals[i]->type))
                arg_vals[i] = xi_lower_checktype_for_type(l, node, arg_vals[i], pt);
            if (i < call->arg_count && call->arguments[i] &&
                call->arguments[i]->type != AST_SPREAD_EXPR) {
                arg_vals[i] = xi_lower_apply_numeric_conversion_witness(l, call->arguments[i],
                                                                        arg_vals[i], pt);
                if (!arg_vals[i])
                    return NULL;
            }
        }
    }

    XiCallWriteback *writebacks = NULL;
    XiCallPlan *call_plan =
        lower_build_call_plan(l, call, arg_vals, n, pmodes, pcount, read_places, read_place_count,
                              callee_type, &writebacks, (int) node->line);
    if (l->had_error)
        return NULL;

    bool is_self_call = (l->self_var_id >= 0 && (uint32_t) l->self_var_id <= XI_MAX_VAR_ID &&
                         callee_val->var_id == (XiVarId) l->self_var_id);

    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *symbol_return_type = NULL;
    if (call->callee && call->callee->type == AST_VARIABLE && l->analyzer) {
        XaSymbol *callee_symbol =
            xa_scope_lookup_by_id(l->analyzer->global_scope, call->callee->as.variable.symbol_id);
        XaSymbolLinks *callee_links =
            callee_symbol ? xa_analyzer_get_links(l->analyzer, callee_symbol) : NULL;
        if (callee_links) {
            symbol_return_type = callee_links->return_type;
            if (!symbol_return_type && callee_links->type &&
                callee_links->type->kind == XR_KIND_FUNCTION)
                symbol_return_type = callee_links->type->function.return_type;
        }
    }
    /* The analyzed call-expression type is authoritative after overload and
     * generic resolution.  Only recover from an absent/incomplete side-table
     * result; replacing a concrete result with the declaration signature loses
     * specialization facts (for example math.min<int> becoming float).
     */
    bool result_type_needs_recovery =
        !result_type || result_type->kind == XR_KIND_UNIT || result_type->kind == XR_KIND_UNKNOWN;
    if (result_type_needs_recovery) {
        if (static_callee && static_callee->return_type)
            result_type = static_callee->return_type;
        else if (symbol_return_type)
            result_type = symbol_return_type;
        else if (callee_type && callee_type->kind == XR_KIND_FUNCTION &&
                 callee_type->function.return_type)
            result_type = callee_type->function.return_type;
    }
    if (math_callee_member && lower_math_call_arity_ok(math_callee_member, n))
        result_type = lower_math_call_result_type(l, math_callee_member, arg_vals, n);

    uint16_t nargs = (uint16_t) (n + 1);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = callee_val;
    for (int i = 0; i < n; i++)
        v->args[i + 1] = arg_vals[i];
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    if (is_self_call)
        v->aux_int = 1;
    if (xi_lower_call_constructs_instance(l, call, result_type))
        v->lowering_flags |= XI_LOWERING_FLAG_CONSTRUCTOR_CALL;
    v->call_plan = call_plan;
    lower_instantiate_call_view_evidence(v, static_callee, callee_type, false);

    xi_lower_bind_callsite_id(l, v, xi_lower_source_node_id(l, node));
    lower_call_emit_err_check(l, v, node, call, callee_type);
    if (!lower_apply_call_writebacks(l, call_plan, writebacks, (int) node->line))
        return NULL;
    return v;
}

static XiValue *lower_enum_method_callee(XiLower *l, const XaSelection *sel, int line) {
    if (!l || !sel || !sel->target_symbol || !sel->receiver_type ||
        sel->receiver_type->kind != XR_KIND_ENUM)
        return NULL;
    const char *enum_name = sel->receiver_type->enum_type.enum_name;
    const char *method_name = sel->target_symbol->name;
    const char *hidden = xi_lower_enum_method_hidden_name(l->func, enum_name, method_name,
                                                          sel->target_symbol->is_static);
    uint32_t sid = sel->target_symbol->id;

    int var_id = xi_lower_var_find(l, sid, hidden);
    if (var_id >= 0) {
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            XiValue *load = xi_lower_emit_top_load(l, b, sel->result_type);
            if (load)
                load->line = (uint32_t) line;
            return load;
        }
        return xi_lower_braun_read(l, var_id, l->cur_block);
    }

    XiTopBinding tb = xi_lower_find_top_binding(l, sid, hidden);
    if (xi_top_binding_valid(tb)) {
        XiValue *load = xi_lower_emit_top_load(l, tb, sel->result_type);
        if (load)
            load->line = (uint32_t) line;
        return load;
    }

    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, hidden, &upval_type);
    if (upval_idx >= 0) {
        XiValue *load = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                                     upval_type ? upval_type : sel->result_type, 0);
        if (load) {
            load->aux_int = upval_idx;
            load->line = (uint32_t) line;
        }
        return load;
    }
    return NULL;
}

static XiValue *lower_enum_method_direct_call(XiLower *l, AstNode *node, CallExprNode *call,
                                              MemberAccessNode *ma) {
    const XaSelection *sel = xa_analyzer_get_selection(l->analyzer, call->callee);
    if (!sel || !sel->target_symbol || sel->target_symbol->kind != XA_SYM_METHOD ||
        !sel->receiver_type || sel->receiver_type->kind != XR_KIND_ENUM)
        return NULL;
    if (sel->kind != XA_SEL_STATIC_MEMBER && sel->kind != XA_SEL_METHOD)
        return NULL;

    bool is_static = sel->target_symbol->is_static || sel->kind == XA_SEL_STATIC_MEMBER;
    XiValue *callee = lower_enum_method_callee(l, sel, (int) node->line);
    if (!callee)
        return NULL;

    XiValue *recv = NULL;
    if (!is_static) {
        recv = xi_lower_expr(l, ma->object);
        if (!recv)
            return NULL;
    }

    XrParamMode stack_sig_modes[64];
    const XrParamMode *pmodes = NULL;
    int pcount = 0;
    if (sel->result_type && sel->result_type->kind == XR_KIND_FUNCTION) {
        pmodes = lower_function_param_modes(
            l, sel->result_type, stack_sig_modes,
            (int) (sizeof(stack_sig_modes) / sizeof(stack_sig_modes[0])), &pcount);
    }

    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, pmodes, pcount, NULL,
                                       0, (int) node->line))
        return NULL;

    int extra = is_static ? 0 : 1;
    int n = args.count + extra;
    uint16_t nargs = (uint16_t) (n + 1);
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = callee;
    int out = 1;
    if (!is_static)
        v->args[out++] = recv;
    for (int i = 0; i < args.count; i++)
        v->args[out + i] = args.items[i];
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    lower_call_emit_err_check(l, v, node, call, sel->result_type);
    return v;
}

static bool lower_is_direct_arg(AstNode *arg) {
    return arg && arg->type != AST_SPREAD_EXPR;
}

static bool lower_endian_member_index(const char *name, int64_t *out_index) {
    if (!name || !out_index)
        return false;
    if (strcmp(name, "Native") == 0) {
        *out_index = XR_ENDIAN_NATIVE;
        return true;
    }
    if (strcmp(name, "LE") == 0) {
        *out_index = XR_ENDIAN_LE;
        return true;
    }
    if (strcmp(name, "BE") == 0) {
        *out_index = XR_ENDIAN_BE;
        return true;
    }
    return false;
}

static bool lower_endian_arg_const(AstNode *arg, int64_t *out_index) {
    if (!arg || !out_index)
        return false;
    if (arg->type == AST_ENUM_ACCESS) {
        EnumAccessNode *ea = &arg->as.enum_access;
        return ea->enum_name && strcmp(ea->enum_name, "Endian") == 0 &&
               lower_endian_member_index(ea->member_name, out_index);
    }
    if (arg->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &arg->as.member_access;
    if (!ma->object || ma->object->type != AST_VARIABLE)
        return false;
    VariableNode *obj = &ma->object->as.variable;
    if (!obj->name || strcmp(obj->name, "Endian") != 0)
        return false;
    return lower_endian_member_index(ma->name, out_index);
}

static XiValue *lower_byte_slice_int_arg(XiLower *l, AstNode *node, XiValue *arg) {
    if (arg && arg->type && xr_is_json_coercion(l->type_int, arg->type))
        return xi_lower_checktype_for_type(l, node, arg, l->type_int);
    return arg;
}

static XiValue *lower_byte_slice_endian_arg(XiLower *l, AstNode *arg) {
    int64_t endian = XR_ENDIAN_NATIVE;
    if (lower_endian_arg_const(arg, &endian))
        return xi_const_int(l->func, l->cur_block, endian, l->type_int);
    return xi_lower_expr(l, arg);
}

static XiValue *lower_mem_access_call(XiLower *l, AstNode *node, CallExprNode *call,
                                      const char *member) {
    if (!l || !node || !call || !member ||
        (strcmp(member, "load") != 0 && strcmp(member, "store") != 0) ||
        call->type_arg_count != 1 || !call->type_args || !call->type_args[0] || !call->arguments)
        return NULL;

    bool is_store = strcmp(member, "store") == 0;
    if ((!is_store && (call->arg_count < 1 || call->arg_count > 3)) ||
        (is_store && (call->arg_count < 3 || call->arg_count > 4)))
        return NULL;

    XrType *target = l->analyzer ? xr_tref_resolve_in_analyzer(l->analyzer, call->type_args[0])
                                 : xr_tref_resolve(l->isolate, call->type_args[0]);
    target = xi_lower_type_or_any(l, target, "mem access type argument", node->line);
    uint8_t code = xr_ffi_type_from_xrtype(target, false);
    if (!xr_ffi_type_is_memory_scalar(code))
        return NULL;

    XiValue *ptr = xi_lower_expr(l, call->arguments[0]);
    if (!ptr || !ptr->type || !xr_type_is_u8_pointer(ptr->type) ||
        (is_store && !ptr->type->ptr_is_mut))
        return NULL;

    XiValue *offset = NULL;
    if (call->arg_count >= 2) {
        offset = xi_lower_expr(l, call->arguments[1]);
        offset = lower_byte_slice_int_arg(l, node, offset);
    } else {
        offset = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    }
    if (!offset)
        return NULL;
    XiValue *addr = xi_lower_ptr_scaled_addr(l, node, ptr, offset, ptr->type, ptr->type);
    if (!addr)
        return NULL;

    if (!is_store) {
        XiValue *endian = call->arg_count >= 3
                              ? lower_byte_slice_endian_arg(l, call->arguments[2])
                              : xi_const_int(l->func, l->cur_block, XR_ENDIAN_NATIVE, l->type_int);
        if (!endian)
            return NULL;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, target, 2);
        if (!v)
            return NULL;
        v->args[0] = addr;
        v->args[1] = endian;
        v->aux_int = (int64_t) xr_ffi_ptr_aux(code, true);
        v->flags |= XI_FLAG_READS_MEM;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *value = xi_lower_expr(l, call->arguments[2]);
    if (!value)
        return NULL;
    value = xi_lower_narrow_for_static_type(l, node, value, target);
    XiValue *endian = call->arg_count >= 4
                          ? lower_byte_slice_endian_arg(l, call->arguments[3])
                          : xi_const_int(l->func, l->cur_block, XR_ENDIAN_NATIVE, l->type_int);
    if (!endian)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_STORE, l->type_unit, 3);
    if (!v)
        return NULL;
    v->args[0] = addr;
    v->args[1] = value;
    v->args[2] = endian;
    v->aux_int = (int64_t) xr_ffi_ptr_aux(code, true);
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
    v->line = (uint32_t) node->line;
    return v;
}

static uint16_t lower_byte_slice_typed_op_for_target(XrType *target, bool is_load) {
    if (!target)
        return 0;
    if (XR_TYPE_IS_INT(target)) {
        switch (target->scalar_rep) {
            case XR_NATIVE_I16:
            case XR_NATIVE_U16:
                return is_load ? XI_BYTE_SLICE_LOAD_U16 : XI_BYTE_SLICE_STORE_U16;
            case XR_NATIVE_I32:
            case XR_NATIVE_U32:
                return is_load ? XI_BYTE_SLICE_LOAD_U32 : XI_BYTE_SLICE_STORE_U32;
            case XR_NATIVE_I64:
            case XR_NATIVE_U64:
                return is_load ? XI_BYTE_SLICE_LOAD_U64 : XI_BYTE_SLICE_STORE_U64;
            default:
                return 0;
        }
    }
    if (XR_TYPE_IS_FLOAT(target)) {
        switch (target->scalar_rep) {
            case XR_NATIVE_F32:
                return is_load ? XI_BYTE_SLICE_LOAD_F32 : XI_BYTE_SLICE_STORE_F32;
            case XR_NATIVE_F64:
                return is_load ? XI_BYTE_SLICE_LOAD_F64 : XI_BYTE_SLICE_STORE_F64;
            default:
                return 0;
        }
    }
    return 0;
}

static XiValue *lower_byte_slice_typed_signed_load_narrow(XiLower *l, AstNode *node, XiValue *value,
                                                          XrType *target) {
    if (!target || !value || !XR_TYPE_IS_INT(target))
        return value;
    switch (target->scalar_rep) {
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
            return xi_lower_narrow_for_static_type(l, node, value, target);
        default:
            return value;
    }
}

static XiValue *lower_byte_slice_typed_call(XiLower *l, AstNode *node, CallExprNode *call,
                                            XaIntrinsicId intrinsic_id, XiValue *recv,
                                            struct XrType *result_type, bool unchecked_access) {
    if (!call->type_args || !call->type_args[0] || call->type_arg_count != 1)
        return NULL;

    bool byte_slice_typed_load = intrinsic_id == XA_INTRINSIC_BYTE_SLICE_LOAD;
    bool byte_slice_typed_store = intrinsic_id == XA_INTRINSIC_BYTE_SLICE_STORE;
    int n = call->arg_count;
    if ((!byte_slice_typed_load || (n != 1 && n != 2)) &&
        (!byte_slice_typed_store || (n != 2 && n != 3)))
        return NULL;
    for (int i = 0; i < n; i++) {
        if (!lower_is_direct_arg(call->arguments[i]))
            return NULL;
    }

    XrType *target = xr_tref_resolve(l->isolate, call->type_args[0]);
    target = xi_lower_type_or_any(l, target, "byte-slice type argument", node->line);
    unchecked_access = unchecked_access && target && XR_TYPE_IS_INT(target);
    uint16_t byte_slice_op = lower_byte_slice_typed_op_for_target(target, byte_slice_typed_load);
    if (!byte_slice_op)
        return NULL;

    XiValue *offset = xi_lower_expr(l, call->arguments[0]);
    if (!offset)
        return NULL;
    offset = lower_byte_slice_int_arg(l, node, offset);
    if (!offset)
        return NULL;

    XiValue *value = NULL;
    if (byte_slice_typed_store) {
        value = xi_lower_expr(l, call->arguments[1]);
        if (!value)
            return NULL;
        if (target && XR_TYPE_IS_INT(target)) {
            value = lower_byte_slice_int_arg(l, node, value);
            if (!value)
                return NULL;
        }
    }

    int endian_arg_index = byte_slice_typed_load ? 1 : 2;
    bool has_explicit_endian = n > endian_arg_index;
    XiValue *endian = has_explicit_endian
                          ? lower_byte_slice_endian_arg(l, call->arguments[endian_arg_index])
                          : xi_const_int(l->func, l->cur_block, XR_ENDIAN_NATIVE, l->type_int);
    if (!endian)
        return NULL;

    uint16_t expected_args = byte_slice_typed_load ? 3 : 4;
    XrType *op_type = byte_slice_typed_store ? l->type_unit : result_type;
    if (byte_slice_typed_load && (!op_type || xi_lower_type_is_unknown(op_type)))
        op_type = target;
    XiValue *v = xi_value_new(l->func, l->cur_block, byte_slice_op, op_type, expected_args);
    if (!v)
        return NULL;
    v->args[0] = recv;
    v->args[1] = offset;
    if (byte_slice_typed_store) {
        v->args[2] = value;
        v->args[3] = endian;
    } else {
        v->args[2] = endian;
    }
    v->line = (uint32_t) node->line;
    v->xa_intrinsic_id = (uint32_t) intrinsic_id;
    if (unchecked_access)
        v->aux_int |= XI_ACCESS_UNCHECKED;
    v->flags = xi_op_default_effects((XiOp) v->op);
    if (!unchecked_access)
        xi_lower_insert_err_check(l, node, true);
    return byte_slice_typed_load ? lower_byte_slice_typed_signed_load_narrow(l, node, v, target)
                                 : v;
}

static bool lower_is_channel_send_boundary_method(const char *method) {
    return method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0 ||
                      strcmp(method, "sendTimeout") == 0);
}

static XrType *xi_raw_pointer_type_namespace(XiLower *l, AstNode *object) {
    if (!l || !object || object->type != AST_NEW_EXPR)
        return NULL;
    NewExprNode *ne = &object->as.new_expr;
    if (ne->module_name || !ne->class_name || ne->type_arg_count != 1 || !ne->type_args ||
        !ne->type_args[0])
        return NULL;
    bool is_mut = false;
    if (strcmp(ne->class_name, "Ptr") == 0) {
        is_mut = false;
    } else if (strcmp(ne->class_name, "MutPtr") == 0) {
        is_mut = true;
    } else {
        return NULL;
    }
    XrType *pointee = xr_tref_resolve(l->isolate, ne->type_args[0]);
    pointee =
        xi_lower_type_or_any(l, pointee, "raw pointer pointee type", object ? object->line : 0);
    if (!pointee)
        pointee = xr_type_new_unknown(l->isolate);
    return xr_type_new_pointer(l->isolate, pointee, is_mut);
}

static XiValue *lower_raw_pointer_static_call(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!l || !node || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name)
        return NULL;
    XrType *ptr_type = xi_raw_pointer_type_namespace(l, ma->object);
    if (!ptr_type)
        return NULL;
    XrType *result_type = xi_lower_node_type(l, node);
    if (!result_type || xi_lower_type_is_unknown(result_type))
        result_type = ptr_type;
    if (strcmp(ma->name, "null") == 0) {
        if (call->arg_count != 0)
            return NULL;
        XiValue *v = xi_const_int(l->func, l->cur_block, 0, result_type);
        if (v)
            v->line = (uint32_t) node->line;
        return v;
    }
    return NULL;
}

static XiValue *lower_channel_send_boundary_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                 const char *method, XiValue *recv) {
    if (!recv || !recv->type || recv->type->kind != XR_KIND_CHANNEL ||
        !lower_is_channel_send_boundary_method(method))
        return NULL;
    int want_args = strcmp(method, "sendTimeout") == 0 ? 2 : 1;
    if (call->arg_count != want_args || !lower_is_direct_arg(call->arguments[0]))
        return NULL;
    if (want_args == 2 && !lower_is_direct_arg(call->arguments[1]))
        return NULL;

    XiValue *payload = NULL;
    uint8_t transfer_mode = XR_TRANSFER_SHARE;
    if (!xi_lower_boundary_transfer_arg(l, call->arguments[0], &payload, &transfer_mode))
        return NULL;
    XiValue *timeout = NULL;
    if (want_args == 2) {
        timeout = xi_lower_expr(l, call->arguments[1]);
        if (!timeout)
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (strcmp(method, "send") == 0) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_SEND, l->type_unit, 2);
        if (!v)
            return NULL;
        v->args[0] = recv;
        v->args[1] = payload;
        xi_chan_send_set_transfer_mode(v, transfer_mode);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND;
        v->line = (uint32_t) node->line;
        return v;
    }

    uint16_t nargs = (uint16_t) (want_args + 1);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = recv;
    v->args[1] = payload;
    if (want_args == 2) {
        v->args[2] = timeout;
    }
    v->aux = (void *) arena_strdup(l->func, method);
    v->aux_int = (int64_t) xi_lower_method_symbol(l, method) << 1;
    xi_chan_send_set_transfer_mode(v, transfer_mode);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    if (xi_lower_method_may_suspend(recv->type, method, want_args))
        v->flags |= XI_FLAG_MAY_SUSPEND;
    v->line = (uint32_t) node->line;
    xi_lower_bind_callsite_id(l, v, xi_lower_source_node_id(l, node));
    xi_lower_insert_err_check(l, node, true);
    return v;
}

static bool lower_map_set_method_key_access_op(struct XrType *receiver_type, const char *method,
                                               int arg_count, uint8_t *out_op) {
    if (out_op)
        *out_op = 0;
    if (!receiver_type || !method || !out_op)
        return false;
    if (XR_TYPE_IS_MAP(receiver_type)) {
        if (arg_count == 1 && strcmp(method, "get") == 0)
            *out_op = XG_KEY_ACCESS_GET;
        else if (arg_count == 1 && strcmp(method, "containsKey") == 0)
            *out_op = XG_KEY_ACCESS_HAS;
        else if (arg_count == 1 && strcmp(method, "delete") == 0)
            *out_op = XG_KEY_ACCESS_DELETE;
        else if (arg_count == 2 && strcmp(method, "set") == 0)
            *out_op = XG_KEY_ACCESS_SET;
        else if (arg_count == 0 && strcmp(method, "clear") == 0)
            *out_op = XG_KEY_ACCESS_CLEAR;
    } else if (XR_TYPE_IS_SET(receiver_type)) {
        if (arg_count == 1 && strcmp(method, "contains") == 0)
            *out_op = XG_KEY_ACCESS_HAS;
        else if (arg_count == 1 && strcmp(method, "add") == 0)
            *out_op = XG_KEY_ACCESS_ADD;
        else if (arg_count == 1 && strcmp(method, "delete") == 0)
            *out_op = XG_KEY_ACCESS_DELETE;
        else if (arg_count == 0 && strcmp(method, "clear") == 0)
            *out_op = XG_KEY_ACCESS_CLEAR;
    }
    return *out_op != 0;
}

static uint8_t lower_json_static_codec_kind(const MemberAccessNode *member) {
    if (!member || !member->name || !member->object || member->object->type != AST_VARIABLE ||
        !member->object->as.variable.name || strcmp(member->object->as.variable.name, "Json") != 0)
        return 0;
    if (strcmp(member->name, "parse") == 0)
        return XG_JSON_CODEC_PARSE;
    if (strcmp(member->name, "decode") == 0)
        return XG_JSON_CODEC_DECODE;
    if (strcmp(member->name, "encode") == 0)
        return XG_JSON_CODEC_ENCODE;
    if (strcmp(member->name, "stringify") == 0)
        return XG_JSON_CODEC_STRINGIFY;
    return 0;
}

static void lower_take_sequence_call_evidence(XiLower *l, const AstNode *node,
                                              const CallExprNode *call,
                                              const MemberAccessNode *member,
                                              XiSequenceEvidenceIds *out_ids) {
    uint8_t capacity_kind = 0;
    uint8_t bulk_kind = 0;
    uint8_t encoding_kind = 0;
    struct XrType *receiver_type;
    bool is_sequence;
    bool is_string_builder;
    if (!out_ids)
        return;
    memset(out_ids, 0, sizeof(*out_ids));
    if (!l || !node || !call || !member || !member->name || !member->object)
        return;

    if (member->object->type == AST_VARIABLE && member->object->as.variable.name &&
        strcmp(member->object->as.variable.name, "string") == 0 &&
        (strcmp(member->name, "fromUtf8") == 0 || strcmp(member->name, "fromUtf8Lossy") == 0) &&
        call->arg_count == 1) {
        XiSequenceEvidenceKinds kinds = {
            .encoding_op_kind = XG_ENCODING_BYTES_TO_STRING,
        };
        xi_lower_take_sequence_evidence_ids(l, (uint32_t) node->line, kinds, out_ids);
        return;
    }

    receiver_type = xi_lower_node_type(l, member->object);
    is_sequence = lower_type_has_sequence_evidence(receiver_type);
    is_string_builder = xr_type_is_builtin_named_class(receiver_type, "StringBuilder");

    if (is_sequence) {
        if (xi_lower_receiver_method_call_matches(receiver_type, member->name, call->arg_count,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH))
            capacity_kind = XG_CAPACITY_PUSH;
        else if (strcmp(member->name, "append") == 0 && call->arg_count >= 1)
            capacity_kind = XG_CAPACITY_APPEND;
        else if (strcmp(member->name, "extend") == 0 && call->arg_count >= 1)
            capacity_kind = XG_CAPACITY_EXTEND;
        else if (xi_lower_receiver_method_call_matches(receiver_type, member->name, call->arg_count,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE))
            capacity_kind = XG_CAPACITY_RESERVE;
        else if (xi_lower_receiver_method_call_matches(receiver_type, member->name, call->arg_count,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_CLEAR))
            capacity_kind = XG_CAPACITY_CLEAR;
        else if (is_string_builder && strcmp(member->name, "clear") == 0 && call->arg_count == 0)
            capacity_kind = XG_CAPACITY_CLEAR;
        else if (xi_lower_receiver_method_call_matches(
                     receiver_type, member->name, call->arg_count,
                     XA_BUILTIN_RECEIVER_METHOD_ARRAY_TO_STRING) ||
                 (is_string_builder && strcmp(member->name, "toString") == 0 &&
                  call->arg_count == 0)) {
            capacity_kind = XG_CAPACITY_TO_STRING;
            if (is_string_builder)
                encoding_kind = XG_ENCODING_BYTES_TO_STRING;
        } else if (xi_lower_receiver_method_call_matches(
                       receiver_type, member->name, call->arg_count,
                       XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM)) {
            capacity_kind = XG_CAPACITY_APPEND;
            bulk_kind = XG_BULK_COPY;
        } else if (xi_lower_receiver_method_call_matches_either(
                       receiver_type, member->name, call->arg_count,
                       XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COPY_FROM,
                       XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COPY_FROM)) {
            bulk_kind = XG_BULK_COPY;
        } else if (xi_lower_receiver_method_call_matches(receiver_type, member->name,
                                                         call->arg_count,
                                                         XA_BUILTIN_RECEIVER_METHOD_ARRAY_FILL) ||
                   xi_lower_receiver_method_call_matches_either(
                       receiver_type, member->name, call->arg_count,
                       XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_FILL,
                       XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_FILL)) {
            bulk_kind = XG_BULK_FILL;
        } else if (xi_lower_receiver_method_call_matches_either(
                       receiver_type, member->name, call->arg_count,
                       XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMPARE,
                       XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COMPARE)) {
            bulk_kind = XG_BULK_COMPARE;
        } else if (xi_lower_receiver_method_call_matches_either(
                       receiver_type, member->name, call->arg_count,
                       XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM,
                       XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_REPEAT_FROM)) {
            bulk_kind = XG_BULK_REPEAT;
        }
    }
    if (receiver_type && receiver_type->kind == XR_KIND_STRING &&
        strcmp(member->name, "copyBytes") == 0 && call->arg_count == 0)
        encoding_kind = XG_ENCODING_STRING_TO_BYTES;

    XiSequenceEvidenceKinds kinds = {
        .capacity_op_kind = capacity_kind,
        .bulk_op_kind = bulk_kind,
        .encoding_op_kind = encoding_kind,
    };
    xi_lower_take_sequence_evidence_ids(l, (uint32_t) node->line, kinds, out_ids);
}

static void lower_take_memory_intrinsic_evidence(XiLower *l, const AstNode *node,
                                                 XaIntrinsicId intrinsic_id,
                                                 XiSequenceEvidenceIds *out_ids) {
    XiSequenceEvidenceKinds kinds = {0};
    if (!out_ids)
        return;
    memset(out_ids, 0, sizeof(*out_ids));
    switch (intrinsic_id) {
        case XA_INTRINSIC_BYTE_SLICE_FILL:
        case XA_INTRINSIC_POD_SLICE_FILL:
            kinds.bulk_op_kind = XG_BULK_FILL;
            break;
        case XA_INTRINSIC_BYTE_SLICE_COPY:
        case XA_INTRINSIC_POD_SLICE_COPY:
            kinds.bulk_op_kind = XG_BULK_COPY;
            break;
        case XA_INTRINSIC_BYTE_SLICE_COMPARE:
        case XA_INTRINSIC_POD_SLICE_COMPARE:
            kinds.bulk_op_kind = XG_BULK_COMPARE;
            break;
        case XA_INTRINSIC_BYTE_SLICE_REPEAT:
            kinds.bulk_op_kind = XG_BULK_REPEAT;
            break;
        default:
            return;
    }
    xi_lower_take_sequence_evidence_ids(l, node ? (uint32_t) node->line : 0, kinds, out_ids);
}

static bool lower_intrinsic_shuffle_pattern(XiLower *l, AstNode *node, CallExprNode *call,
                                            const XaIntrinsicDesc *desc, int64_t *extra) {
    if (!l || !call || !desc || !extra)
        return false;
    uint8_t lanes = desc->shape_rule.input_lanes;
    if ((desc->flags & XA_INTRINSIC_FLAG_EXPLICIT_SHUFFLE) != 0) {
        if (lanes > XI_VEC_SHAPE_PACKED_SHUFFLE_LANES) {
            l->had_error = true;
            return false;
        }
        for (uint8_t lane = 0; lane < lanes; lane++) {
            int64_t selected = -1;
            const char *error = NULL;
            AstNode *arg = lane < call->arg_count ? call->arguments[lane] : NULL;
            if (!arg || !xa_consteval_int_expr(l->analyzer, arg, &selected, &error) ||
                selected < 0 || selected >= lanes) {
                (void) error;
                l->had_error = true;
                return false;
            }
            *extra |= selected << (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4);
        }
    } else if ((desc->flags & XA_INTRINSIC_FLAG_SWAP_ADJACENT) != 0) {
        *extra |= XI_VEC_SHAPE_SWAP_ADJACENT;
        if (lanes <= XI_VEC_SHAPE_PACKED_SHUFFLE_LANES) {
            for (uint8_t lane = 0; lane < lanes; lane++)
                *extra |= (int64_t) (lane ^ 1u) << (XI_VEC_SHAPE_SHUFFLE_SHIFT + lane * 4);
        }
    } else if ((desc->flags & XA_INTRINSIC_FLAG_SWAP_LANES) != 0) {
        if (lanes != 2) {
            l->had_error = true;
            return false;
        }
        *extra |= INT64_C(1) << XI_VEC_SHAPE_SHUFFLE_SHIFT;
    }
    (void) node;
    return true;
}

static bool lower_codegen_opaque_type_supported(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    if (type->kind == XR_KIND_POINTER)
        return true;
    if (type->kind != XR_KIND_INT)
        return false;
    switch ((XrNativeType) type->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static XiValue *lower_codegen_intrinsic_call(XiLower *l, AstNode *node, CallExprNode *call,
                                             const XaIntrinsicDesc *desc) {
    if (!l || !node || !call || !desc || desc->family != XA_INTRINSIC_FAMILY_CODEGEN)
        return NULL;
    XiOp op = xi_semantic_intrinsic_op(desc);
    uint16_t nargs = (uint16_t) call->arg_count;
    XrType *result_type = xi_lower_node_type(l, node);
    if (op == XI_CODEGEN_OPAQUE) {
        if (nargs != 1 || !call->arguments[0] ||
            !lower_codegen_opaque_type_supported(result_type)) {
            l->had_error = true;
            return NULL;
        }
    } else if (op != XI_CODEGEN_COMPILER_FENCE || nargs != 0) {
        l->had_error = true;
        return NULL;
    }
    /* Lower operands before inserting their user into the block.  Bytecode
     * emission follows block value order, so creating CODEGEN_OPAQUE first
     * would make opaque(integer-literal) read the literal's register before
     * the CONST instruction initialized it. */
    XiValue *args[1] = {NULL};
    for (uint16_t i = 0; i < nargs; i++) {
        args[i] = xi_lower_expr(l, call->arguments[i]);
        if (!args[i])
            return NULL;
    }
    XiValue *value = xi_value_new(l->func, l->cur_block, op, result_type, nargs);
    if (!value)
        return NULL;
    for (uint16_t i = 0; i < nargs; i++)
        value->args[i] = args[i];
    value->xa_intrinsic_id = (uint32_t) desc->id;
    value->flags = xi_op_default_effects(op);
    value->line = (uint32_t) node->line;
    return value;
}

static XiValue *lower_resolved_intrinsic_call(XiLower *l, AstNode *node, CallExprNode *call,
                                              const XaResolvedCall *resolved) {
    if (!l || !node || !call || !resolved || resolved->reason != XA_RESOLVED_CALL_REASON_RESOLVED)
        return NULL;
    const XaIntrinsicDesc *desc = xa_intrinsic_by_id(resolved->intrinsic_id);
    if (!desc || call->arg_count < desc->min_arity || call->arg_count > desc->max_arity ||
        !call->callee || call->callee->type != AST_MEMBER_ACCESS) {
        l->had_error = true;
        return NULL;
    }
    if (desc->family == XA_INTRINSIC_FAMILY_CODEGEN)
        return lower_codegen_intrinsic_call(l, node, call, desc);
    MemberAccessNode *member = &call->callee->as.member_access;
    XiOp op = xi_semantic_intrinsic_op(desc);
    bool typed_byte_slice = desc->lowering == XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_LOAD ||
                            desc->lowering == XA_INTRINSIC_LOWERING_BYTE_SLICE_TYPED_STORE;
    if ((!typed_byte_slice && op == XI_OP_COUNT) || !member->object) {
        l->had_error = true;
        return NULL;
    }

    if (call->arg_count > XI_LOWER_MAX_CALL_ARGS) {
        l->had_error = true;
        return NULL;
    }
    XiValue *receiver = xi_lower_expr(l, member->object);
    if (!receiver)
        return NULL;
    if (typed_byte_slice)
        return lower_byte_slice_typed_call(
            l, node, call, resolved->intrinsic_id, receiver, xi_lower_node_type(l, node),
            (resolved->flags & XA_RESOLVED_CALL_FLAG_UNSAFE_SCOPE) != 0);

    XrParamMode stack_modes[64];
    const XrParamMode *param_modes = NULL;
    int param_count = 0;
    XrType *method_type = xr_type_non_nullable(l->isolate, xi_lower_node_type(l, call->callee));
    if (method_type && method_type->kind == XR_KIND_FUNCTION) {
        param_modes = lower_function_param_modes(
            l, method_type, stack_modes, (int) (sizeof(stack_modes) / sizeof(stack_modes[0])),
            &param_count);
    }

    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, param_modes,
                                       param_count, NULL, 0, (int) node->line))
        return NULL;

    uint16_t nargs = (uint16_t) (args.count + 1);
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *value = xi_value_new(l->func, l->cur_block, op, result_type, nargs);
    if (!value)
        return NULL;
    value->args[0] = receiver;
    for (int i = 0; i < args.count; i++)
        value->args[i + 1] = args.items[i];

    int64_t extra = 0;
    if ((desc->flags & XA_INTRINSIC_FLAG_ODD_LANES) != 0)
        extra |= XI_VEC_SHAPE_ODD_LANES;
    if ((desc->flags & XA_INTRINSIC_FLAG_UNZIP) != 0)
        extra |= XI_VEC_SHAPE_UNZIP;
    if ((desc->flags & XA_INTRINSIC_FLAG_CONTIGUOUS_HALF) != 0)
        extra |= XI_VEC_SHAPE_CONTIGUOUS_HALF;
    if ((desc->flags & XA_INTRINSIC_FLAG_SCALABLE) != 0)
        extra |= XI_VEC_SHAPE_SCALABLE;
    if (desc->lowering == XA_INTRINSIC_LOWERING_VEC_SHUFFLE &&
        !lower_intrinsic_shuffle_pattern(l, node, call, desc, &extra))
        return NULL;
    if (desc->family == XA_INTRINSIC_FAMILY_SIMD) {
        value->aux_int = xi_vec_shape_encode(desc->shape_rule.result_native_type,
                                             desc->shape_rule.result_lanes) |
                         extra;
        bool unchecked_access = (resolved->flags & XA_RESOLVED_CALL_FLAG_UNSAFE_SCOPE) != 0 &&
                                (op == XI_VEC_LOAD || op == XI_VEC_STORE);
        if (unchecked_access)
            value->aux_int |= XI_ACCESS_UNCHECKED;
    } else if (desc->family == XA_INTRINSIC_FAMILY_BITS) {
        value->aux_int = receiver->type ? receiver->type->scalar_rep : 0;
    }
    value->xa_intrinsic_id = (uint32_t) desc->id;
    value->flags = xi_op_default_effects(op);
    value->line = (uint32_t) node->line;
    if (desc->family == XA_INTRINSIC_FAMILY_MEMORY) {
        if (desc->lowering == XA_INTRINSIC_LOWERING_SLICE_REINTERPRET) {
            if (call->type_arg_count != 1 || !call->type_args || !call->type_args[0]) {
                l->had_error = true;
                return NULL;
            }
            XrType *target = xr_tref_resolve_in_analyzer(l->analyzer, call->type_args[0]);
            target = xi_lower_type_or_any(l, target, "span reinterpret type argument",
                                          node ? node->line : 0);
            int64_t reinterpret_aux = xi_pack_span_elem_aux(l, target);
            if (reinterpret_aux == 0) {
                l->had_error = true;
                return NULL;
            }
            value->aux_int = reinterpret_aux;
            value->aux = xi_lower_type_struct_layout(l, target);
        } else if (desc->lowering == XA_INTRINSIC_LOWERING_SLICE_GET) {
            value->aux_int = 1;
        } else if ((desc->lowering == XA_INTRINSIC_LOWERING_SLICE_WINDOW ||
                    desc->lowering == XA_INTRINSIC_LOWERING_BYTE_SLICE_COPY ||
                    desc->lowering == XA_INTRINSIC_LOWERING_SLICE_COPY) &&
                   (resolved->flags & XA_RESOLVED_CALL_FLAG_UNSAFE_SCOPE) != 0) {
            value->aux_int |= XI_ACCESS_UNCHECKED;
        }
        XiSequenceEvidenceIds sequence_ids;
        lower_take_memory_intrinsic_evidence(l, node, resolved->intrinsic_id, &sequence_ids);
        xi_lower_apply_sequence_evidence_ids(value, &sequence_ids);
    }
    bool unchecked_access = (value->aux_int & XI_ACCESS_UNCHECKED) != 0;
    if (!unchecked_access && (desc->effect == XA_INTRINSIC_EFFECT_MAY_THROW ||
                              desc->effect == XA_INTRINSIC_EFFECT_READ_MAY_THROW ||
                              desc->effect == XA_INTRINSIC_EFFECT_WRITE_MAY_THROW))
        xi_lower_insert_err_check(l, node, true);
    return value;
}

static XrType *lower_known_expr_type(XiLower *l, AstNode *node) {
    XrType *type = l && node ? xi_lower_node_type(l, node) : NULL;
    if (type && !xi_lower_type_is_unknown(type))
        return type;
    if (!l || !node || node->type != AST_VARIABLE)
        return NULL;
    for (XiLower *scope = l; scope; scope = scope->parent) {
        int var_id = xi_lower_var_find(scope, node->as.variable.symbol_id, node->as.variable.name);
        if (var_id >= 0 && var_id < scope->var_count && scope->vars[var_id].type)
            return scope->vars[var_id].type;
    }
    XiTopBinding top =
        xi_lower_find_top_binding(l, node->as.variable.symbol_id, node->as.variable.name);
    return xi_top_binding_valid(top) ? top.type : NULL;
}

static XiValue *lower_call(XiLower *l, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;

    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &call->callee->as.member_access;
        bool mem_object = lower_call_object_is_module(l, member->object, "mem") ||
                          (member->object && member->object->type == AST_VARIABLE &&
                           member->object->as.variable.name &&
                           strcmp(member->object->as.variable.name, "mem") == 0);
        if (member->name && mem_object) {
            XiValue *with_slice_mut = lower_mem_with_slice_mut_call(l, node, call, member->name);
            if (with_slice_mut)
                return with_slice_mut;
        }
    }

    const XaResolvedCall *resolved =
        l && l->typed_program ? xa_typed_program_resolved_call(l->typed_program, node) : NULL;
    XaResolvedCall lowering_resolved;
    if (!resolved && call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &call->callee->as.member_access;
        XaIntrinsicId intrinsic_id = xa_intrinsic_compiler_receiver_method(
            lower_known_expr_type(l, member->object), member->name);
        if (intrinsic_id != XA_INTRINSIC_NONE) {
            lowering_resolved = (XaResolvedCall) {
                .source_node_id = node->node_id,
                .intrinsic_id = intrinsic_id,
                .reason = XA_RESOLVED_CALL_REASON_RESOLVED,
            };
            resolved = &lowering_resolved;
        }
    }
    const XaIntrinsicDesc *resolved_desc =
        resolved ? xa_intrinsic_by_id(resolved->intrinsic_id) : NULL;
    if (resolved_desc && resolved_desc->family != XA_INTRINSIC_FAMILY_PARALLEL)
        return lower_resolved_intrinsic_call(l, node, call, resolved);

    /* The source keyword is lowercase, while the runtime's builtin class is
     * named String. Resolve canonical static constructors to that class. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *static_ma = &call->callee->as.member_access;
        if (static_ma->object && static_ma->object->type == AST_VARIABLE && static_ma->name &&
            strcmp(static_ma->object->as.variable.name, "string") == 0) {
            bool is_utf8_static = strcmp(static_ma->name, "fromUtf8") == 0 ||
                                  strcmp(static_ma->name, "fromUtf8Lossy") == 0;
            bool is_from_rune = strcmp(static_ma->name, "fromRune") == 0;
            bool is_join = strcmp(static_ma->name, "join") == 0;
            if ((is_utf8_static || is_from_rune || is_join) &&
                ((is_join && (call->arg_count == 1 || call->arg_count == 2)) ||
                 (!is_join && call->arg_count == 1))) {
                XiSequenceEvidenceIds sequence_ids;
                lower_take_sequence_call_evidence(l, node, call, static_ma, &sequence_ids);
                XiValue *recv = xi_lower_emit_builtin_class(l, "String", node->line);
                if (!recv)
                    return NULL;
                struct XrType *result_type = xi_lower_node_type(l, node);
                if (!result_type || xi_lower_type_is_unknown(result_type)) {
                    result_type = strcmp(static_ma->name, "fromUtf8") == 0
                                      ? xr_type_new_optional(l->isolate, l->type_string)
                                      : l->type_string;
                }
                XiValue *arg_vals[2] = {0};
                for (int i = 0; i < call->arg_count; i++) {
                    AstNode *arg_node = call->arguments[i];
                    bool elided_full_slice = false;
                    if (is_utf8_static && i == 0 && arg_node && arg_node->type == AST_SLICE_EXPR &&
                        !arg_node->as.slice_expr.start && !arg_node->as.slice_expr.end) {
                        XiSequenceEvidenceIds ignored_slice_ids;
                        XiSequenceEvidenceKinds slice_kinds = {
                            .sequence_access_kind = XG_SEQ_ACCESS_SLICE,
                        };
                        xi_lower_take_sequence_evidence_ids(l, (uint32_t) arg_node->line,
                                                            slice_kinds, &ignored_slice_ids);
                        arg_node = arg_node->as.slice_expr.source;
                        elided_full_slice = true;
                    }
                    XiValue *arg = xi_lower_expr(l, arg_node);
                    if (!arg)
                        return NULL;
                    arg_vals[i] = arg;
                    if (elided_full_slice) {
                        XiValue *retain =
                            xi_value_new(l->func, l->cur_block, XI_RETAIN, l->type_unit, 1);
                        if (!retain)
                            return NULL;
                        retain->args[0] = arg;
                        retain->flags |= XI_FLAG_SIDE_EFFECT;
                        retain->line = (uint32_t) node->line;
                    }
                }
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type,
                                          (uint16_t) (call->arg_count + 1));
                if (!v)
                    return NULL;
                v->args[0] = recv;
                for (int i = 0; i < call->arg_count; i++)
                    v->args[i + 1] = arg_vals[i];
                v->aux = (void *) arena_strdup(l->func, static_ma->name);
                v->aux_int = (int64_t) xi_lower_method_symbol(l, static_ma->name) << 1;
                v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
                v->line = (uint32_t) node->line;
                xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
                xi_lower_insert_err_check(l, node, true);
                return v;
            }
        }
    }

    if (lower_call_is_sys_thread_spawn(call))
        return lower_thread_spawn_call(l, node, call);

    XiValue *raw_pointer_static = lower_raw_pointer_static_call(l, node, call);
    if (raw_pointer_static)
        return raw_pointer_static;

    /* Method call: callee is obj.method — emit XI_CALL_METHOD (→ OP_INVOKE).
     * This is required for builtin methods (set.size, array.push, etc.)
     * which rely on OP_INVOKE dispatch rather than GETPROP + CALL. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;
        uint8_t json_codec_kind = lower_json_static_codec_kind(ma);
        XiSequenceEvidenceIds sequence_ids;
        lower_take_sequence_call_evidence(l, node, call, ma, &sequence_ids);

        XiValue *enum_direct = lower_enum_method_direct_call(l, node, call, ma);
        if (enum_direct)
            return enum_direct;

        if (ma->object && ma->object->type == AST_VARIABLE && ma->name &&
            strcmp(ma->name, "withCapacity") == 0 && call->arg_count == 1 &&
            strcmp(ma->object->as.variable.name, "Array") == 0) {
            XiValue *cap = xi_lower_expr(l, call->arguments[0]);
            if (!cap)
                return NULL;
            struct XrType *result_type = xi_lower_node_type(l, node);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            v->aux = (void *) "array_with_capacity";
            v->aux_int = xi_array_cfield_from_type(result_type);
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }

        /* Json.decode<T>(data) → XI_JSON_DECODE with compile-time field info.
         * The analyzer already validated T is a sealed Record type with fields
         * and stored the result type as T? in the node table. */
        if (ma->name && strcmp(ma->name, "decode") == 0 && ma->object &&
            ma->object->type == AST_VARIABLE && strcmp(ma->object->as.variable.name, "Json") == 0 &&
            call->type_arg_count == 1 && call->arg_count == 1) {
            struct XrType *result_type = xi_lower_node_type(l, node);
            if (result_type && XR_TYPE_IS_RECORD(result_type) && result_type->object.is_sealed &&
                result_type->object.field_count > 0) {
                int fc = result_type->object.field_count;
                XiValue *data_val = xi_lower_expr(l, call->arguments[0]);
                if (!data_val)
                    return NULL;

                /* Arena-copy field names so they survive AST destruction */
                const char **names = (const char **) xi_func_arena_alloc(
                    l->func, (uint32_t) (fc * (int) sizeof(const char *)));
                XR_DCHECK(names != NULL, "json_decode: arena alloc failed");
                for (int i = 0; i < fc; i++) {
                    names[i] = arena_strdup(l->func, result_type->object.field_names[i]);
                }

                XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_DECODE, result_type, 1);
                if (!v)
                    return NULL;
                v->args[0] = data_val;
                v->aux = (void *) names;
                v->aux_int = fc;
                v->flags |= XI_FLAG_SIDE_EFFECT;
                v->line = (uint32_t) node->line;
                xi_lower_bind_json_codec_id(l, v, xi_lower_source_node_id(l, node),
                                            XG_JSON_CODEC_DECODE);
                return v;
            }
        }

        /* Coro.method() → XI_CORO_OP with sub-type encoding.
         * Coro is a built-in module with dedicated VM opcodes; it has
         * no runtime object, so the generic XI_CALL_METHOD path would
         * fail because lower_variable("Coro") cannot resolve. */
        if (ma->object && ma->object->type == AST_VARIABLE && ma->name &&
            strcmp(ma->object->as.variable.name, "Coro") == 0) {
            XiValue *coro_op = lower_coro_method(l, node, ma->name, call);
            if (coro_op)
                return coro_op;
            /* Unknown Coro method — fall through to generic path which
             * will report "unresolved variable" for Coro. */
        }

        if (ma->object && ma->object->type == AST_VARIABLE && ma->name &&
            strcmp(ma->object->as.variable.name, "CoroPool") == 0 &&
            strcmp(ma->name, "submit") == 0) {
            XiValue *task = lower_coro_pool_submit(l, node, call);
            if (task)
                return task;
        }

        if (ma->name && lower_call_object_is_module(l, ma->object, "mem")) {
            XiValue *layout_const = lower_mem_layout_call(l, node, call, ma->name);
            if (layout_const)
                return layout_const;
            XiValue *pointer = lower_mem_pointer_constructor_call(l, node, call, ma->name);
            if (pointer)
                return pointer;
            XiValue *slice = lower_mem_slice_call(l, node, call, ma->name);
            if (slice)
                return slice;
            XiValue *materialized = lower_mem_assume_initialized_call(l, node, call, ma->name);
            if (materialized)
                return materialized;
            XiValue *with_slice_mut = lower_mem_with_slice_mut_call(l, node, call, ma->name);
            if (with_slice_mut)
                return with_slice_mut;
            XiValue *addr = lower_mem_addr_pointer_call(l, node, call, ma->name);
            if (addr)
                return addr;
            XiValue *access = lower_mem_access_call(l, node, call, ma->name);
            if (access)
                return access;
        }

        const XaParallelCallPlan *analyzer_parallel_plan =
            lower_analyzer_parallel_call_plan(l, node);
        if (analyzer_parallel_plan && !analyzer_parallel_plan->is_plan_method) {
            XiValue *parallel_intrinsic = lower_parallel_module_intrinsic_or_error(
                l, node, call, analyzer_parallel_plan->kind, analyzer_parallel_plan->intrinsic_id);
            if (parallel_intrinsic || l->had_error)
                return parallel_intrinsic;
        }

        struct XrType *method_receiver_type = ma->object ? xi_lower_node_type(l, ma->object) : NULL;

        if (ma->name && strcmp(ma->name, "ptr") == 0 && call->arg_count == 0 && ma->object &&
            ma->object->type == AST_FIXED_BYTES_LITERAL) {
            return lower_static_bytes_literal_ptr(l, node, ma->object);
        }

        uint8_t method_key_access_op = 0;
        uint32_t method_key_access_ordinal = UINT32_MAX;
        if (lower_map_set_method_key_access_op(method_receiver_type, ma->name, call->arg_count,
                                               &method_key_access_op)) {
            method_key_access_ordinal =
                xi_lower_next_key_access_ordinal(l, (uint32_t) node->line, method_key_access_op);
        }

        XiValue *recv = xi_lower_expr(l, ma->object);
        if (!recv)
            return NULL;

        if (analyzer_parallel_plan && analyzer_parallel_plan->is_plan_method) {
            XiValue *parallel_plan_intrinsic = lower_parallel_plan_intrinsic_call(
                l, node, call, recv, analyzer_parallel_plan->kind,
                analyzer_parallel_plan->intrinsic_id);
            if (parallel_plan_intrinsic || l->had_error)
                return parallel_plan_intrinsic;
        }

        if (lower_value_is_whole_module_import(l, recv, "sync") &&
            xi_lower_builtin_class_global_index(ma->name) >= 0) {
            return lower_construct(l, node, xi_lower_node_type(l, node), "sync", ma->name,
                                   call->arguments, call->arg_accesses, call->arg_count);
        }

        if (lower_value_is_whole_module_import(l, recv, "mem") && ma->name) {
            XiValue *layout_const = lower_mem_layout_call(l, node, call, ma->name);
            if (layout_const)
                return layout_const;
            XiValue *pointer = lower_mem_pointer_constructor_call(l, node, call, ma->name);
            if (pointer)
                return pointer;
            XiValue *slice = lower_mem_slice_call(l, node, call, ma->name);
            if (slice)
                return slice;
            XiValue *materialized = lower_mem_assume_initialized_call(l, node, call, ma->name);
            if (materialized)
                return materialized;
            XiValue *with_slice_mut = lower_mem_with_slice_mut_call(l, node, call, ma->name);
            if (with_slice_mut)
                return with_slice_mut;
            XiValue *addr = lower_mem_addr_pointer_call(l, node, call, ma->name);
            if (addr)
                return addr;
            XiValue *access = lower_mem_access_call(l, node, call, ma->name);
            if (access)
                return access;
        }

        XiValue *chan_send = lower_channel_send_boundary_call(l, node, call, ma->name, recv);
        if (chan_send)
            return chan_send;

        struct XrType *result_type = xi_lower_node_type(l, node);

        /* Resolve the method's source-argument contract before lowering its
         * arguments.  Value aggregates use ordinary read-copy semantics only
         * for XR_PARAM_READ value slots; ref/move slots and the internal
         * fixed-layout read-place ABI must keep the caller's original place.
         *
         * Ordinary function calls already establish this contract before
         * lower_call_args_expand_spread().  Doing it later for methods caused
         * e.g. `state.digestLong(ref acc)` to deep-clone a stack fixed array and
         * then borrow the clone, adding allocation and hiding the native place
         * from AOT while preserving only accidentally-correct copy-out
         * semantics. */
        XrParamMode stack_method_modes[64];
        const XrParamMode *method_modes = NULL;
        int method_pcount = 0;
        XrType *method_type = xr_type_non_nullable(l->isolate, xi_lower_node_type(l, call->callee));
        if (method_type && method_type->kind == XR_KIND_FUNCTION) {
            method_modes = lower_function_param_modes(
                l, method_type, stack_method_modes,
                (int) (sizeof(stack_method_modes) / sizeof(stack_method_modes[0])), &method_pcount);
        }
        bool stack_method_read_places[64];
        bool *method_read_places = NULL;
        int method_read_place_count = method_pcount;
        if (method_read_place_count > 0) {
            method_read_places =
                method_read_place_count <= (int) (sizeof(stack_method_read_places) /
                                                  sizeof(stack_method_read_places[0]))
                    ? stack_method_read_places
                    : (bool *) xi_func_arena_alloc(
                          l->func, (uint32_t) ((size_t) method_read_place_count * sizeof(bool)));
            if (!method_read_places)
                return NULL;
            lower_collect_read_place_params(l, NULL, method_type, method_modes, method_pcount,
                                            method_read_places, method_read_place_count);
        }

        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerArgList args;
        xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, method_modes,
                                           method_pcount, method_read_places,
                                           method_read_place_count, (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;
        bool is_time_sleep = n == 1 && ma->name && strcmp(ma->name, "sleep") == 0 &&
                             lower_value_is_whole_module_import(l, recv, "time");

        if (recv->type && xr_type_is_builtin_named_class(recv->type, "CoroLocal") && ma->name) {
            int sub = -1;
            if (strcmp(ma->name, "set") == 0 && n == 1)
                sub = XI_CORO_SUB_SET_LOCAL;
            else if (strcmp(ma->name, "get") == 0 && n == 0)
                sub = XI_CORO_SUB_GET_LOCAL;
            if (sub >= 0) {
                XiValue *local = xi_value_new(l->func, l->cur_block, XI_CORO_OP, result_type,
                                              (uint16_t) (n + 1));
                if (!local)
                    return NULL;
                local->args[0] = recv;
                for (int i = 0; i < n; i++)
                    local->args[i + 1] = arg_vals[i];
                local->aux_int = sub;
                local->flags |= XI_FLAG_SIDE_EFFECT;
                local->line = (uint32_t) node->line;
                return local;
            }
        }

        if (lower_value_is_whole_module_import(l, recv, "math") &&
            lower_math_call_arity_ok(ma->name, n))
            result_type = lower_math_call_result_type(l, ma->name, arg_vals, n);

        uint16_t wrap_arith_op = xi_lower_int_wrapping_method_op(recv->type, ma->name, n);
        if (wrap_arith_op != XI_OP_COUNT && arg_vals[0] && arg_vals[0]->type &&
            XR_TYPE_IS_INT(arg_vals[0]->type)) {
            return xi_lower_int_wrapping_method(l, node, wrap_arith_op, recv, arg_vals[0]);
        }

        if (xi_lower_receiver_method_call_matches(recv->type, ma->name, n,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_CLEAR)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->aux = (void *) "array_clear";
            v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            v->line = (uint32_t) node->line;
            xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "span") == 0 && n == 0) {
            XiValue *start = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *end = xi_const_int(l->func, l->cur_block, INT64_MAX, l->type_int);
            struct XrType *span_type = result_type;
            if (!span_type || xi_lower_type_is_unknown(span_type)) {
                XrType *elem = xi_get_container_elem_type(recv->type);
                span_type =
                    xr_type_new_slice(l->isolate, elem ? elem : xr_type_new_unknown(l->isolate));
            }
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SLICE, span_type, 3);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = start;
            v->args[2] = end;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && recv->type->kind == XR_KIND_STRING && ma->name &&
            strcmp(ma->name, "bytes") == 0 && n == 0) {
            struct XrType *byte_slice_type = result_type;
            if (!byte_slice_type || xi_lower_type_is_unknown(byte_slice_type))
                byte_slice_type = xr_type_new_u8_slice(l->isolate);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, byte_slice_type, 1);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->aux = (void *) "string_byte_slice";
            v->flags |= XI_FLAG_READS_MEM | XI_FLAG_MAY_THROW;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (xi_lower_receiver_method_call_matches(recv->type, ma->name, n,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->aux = (void *) "array_reserve";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
            return v;
        }

        if (xi_lower_receiver_method_call_matches(recv->type, ma->name, n,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESIZE)) {
            XiValue *fill = arg_vals[1];
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 3);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->args[2] = fill;
            v->aux = (void *) "array_resize";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (xi_lower_receiver_method_call_matches(
                recv->type, ma->name, n, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM)) {
            XiValue *v =
                xi_value_new(l->func, l->cur_block, XI_BYTE_ARRAY_APPEND_FROM, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->line = (uint32_t) node->line;
            xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
            return v;
        }

        if (xi_lower_receiver_method_call_matches(
                recv->type, ma->name, n, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM)) {
            for (int i = 0; i < n; i++) {
                if (arg_vals[i] && arg_vals[i]->type &&
                    xr_is_json_coercion(l->type_int, arg_vals[i]->type))
                    arg_vals[i] = xi_lower_checktype_for_type(l, node, arg_vals[i], l->type_int);
            }
            XiValue *v =
                xi_value_new(l->func, l->cur_block, XI_BYTE_ARRAY_REPEAT_FROM, result_type, 3);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->args[2] = arg_vals[1];
            v->line = (uint32_t) node->line;
            xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "get") == 0 && n == 1) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->aux_int = 1;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "set") == 0 && n == 2) {
            struct XrType *elem_type = xi_get_container_elem_type(recv->type);
            arg_vals[1] = xi_lower_narrow_for_static_type(l, node, arg_vals[1], elem_type);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, result_type, 3);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->args[2] = arg_vals[1];
            v->aux_int = 1;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type &&
            (XR_TYPE_IS_ARRAY(recv->type) || recv->type->kind == XR_KIND_FIXED_ARRAY) && ma->name &&
            n == 0 &&
            (strcmp(ma->name, "ptr") == 0 ||
             (recv->type->kind != XR_KIND_FIXED_ARRAY && strcmp(ma->name, "mutPtr") == 0))) {
            struct XrType *pointer_type = result_type;
            if (!pointer_type || !XR_TYPE_IS_POINTER(pointer_type)) {
                struct XrType *element_type = xi_get_container_elem_type(recv->type);
                pointer_type = xr_type_new_pointer(
                    l->isolate, element_type ? element_type : xr_type_new_unknown(l->isolate),
                    strcmp(ma->name, "mutPtr") == 0);
            }
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_ARRAY_DATA_PTR, pointer_type, 1);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->line = (uint32_t) node->line;
            return v;
        }

        /* FFI raw pointer methods: deref()/offset(i), copy, and isNull(). */
        if (recv->type && XR_TYPE_IS_POINTER(recv->type) && ma->name) {
            if (strcmp(ma->name, "deref") == 0 && n == 0) {
                struct XrType *pointee_type = recv->type->container.element_type;
                if (!pointee_type || XR_TYPE_IS_UNKNOWN(pointee_type))
                    pointee_type = result_type;
                XiValue *endian =
                    xi_const_int(l->func, l->cur_block, XR_ENDIAN_NATIVE, l->type_int);
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, pointee_type, 2);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->args[1] = endian;
                v->aux_int = (int64_t) xr_ffi_ptr_aux(xi_pointer_pointee_ffi(recv->type), false);
                v->flags |= XI_FLAG_READS_MEM;
                v->line = (uint32_t) node->line;
                return v;
            }
            if (strcmp(ma->name, "offset") == 0 && n == 1) {
                XiValue *v =
                    xi_lower_ptr_scaled_addr(l, node, recv, arg_vals[0], recv->type, recv->type);
                if (!v)
                    return NULL;
                return v;
            }
            if (strcmp(ma->name, "copyFromNonOverlapping") == 0 && n == 2) {
                XiValue *byte_count = arg_vals[1];
                int64_t size = xi_pointer_pointee_size(l, recv->type);
                if (size != 1) {
                    XiValue *sz = xi_const_int(l->func, l->cur_block, size, l->type_int);
                    XiValue *mul = xi_value_new(l->func, l->cur_block, XI_MUL, l->type_int, 2);
                    if (!mul)
                        return NULL;
                    mul->args[0] = byte_count;
                    mul->args[1] = sz;
                    mul->line = (uint32_t) node->line;
                    byte_count = mul;
                }
                XiValue *v =
                    xi_value_new(l->func, l->cur_block, XI_PTR_COPY_NONOVERLAP, result_type, 3);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->args[1] = arg_vals[0];
                v->args[2] = byte_count;
                v->line = (uint32_t) node->line;
                return v;
            }
            if (strcmp(ma->name, "isNull") == 0 && n == 0) {
                XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_EQ, l->type_bool, 2);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->args[1] = zero;
                v->line = (uint32_t) node->line;
                return v;
            }
        }

        xi_lower_check_map_method_args(l, node, ma->name, recv, arg_vals, n);
        xi_lower_check_set_method_args(l, node, ma->name, recv, arg_vals, n);
        xi_lower_narrow_map_method_args(l, node, ma->name, recv, arg_vals, n);
        xi_lower_narrow_set_method_args(l, node, ma->name, recv, arg_vals, n);

        if (recv->type && recv->type->kind == XR_KIND_CHANNEL && ma->name &&
            strcmp(ma->name, "send") == 0 && n == 1) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_SEND, l->type_unit, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && recv->type->kind == XR_KIND_CHANNEL && ma->name &&
            strcmp(ma->name, "recvOr") == 0 && n == 1) {
            /* ch.recvOr(fallback): blocking recv returning the received value,
             * or `fallback` when the channel is closed and drained. Reuses the
             * raw XI_CHAN_RECV / XI_CHAN_RECV_STATUS fast path (same as the recv
             * match lowering), so no Recv<T> enum is materialized and VM / AOT
             * agree. Equivalent to
             *   match ch.recv() { Recv.Value(v) -> v; _ -> fallback }. */
            struct XrType *payload_type = recv->type->container.element_type
                                              ? recv->type->container.element_type
                                              : result_type;
            XiValue *chan_recv = xi_value_new(l->func, l->cur_block, XI_CHAN_RECV, payload_type, 1);
            if (!chan_recv)
                return NULL;
            chan_recv->args[0] = recv;
            chan_recv->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
            chan_recv->line = (uint32_t) node->line;

            XiValue *status =
                xi_value_new(l->func, l->cur_block, XI_CHAN_RECV_STATUS, l->type_bool, 1);
            if (!status)
                return NULL;
            status->args[0] = chan_recv;
            status->line = (uint32_t) node->line;

            XiBlock *value_blk = xi_block_new(l->func);
            XiBlock *fallback_blk = xi_block_new(l->func);
            XiBlock *merge = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, status, value_blk, fallback_blk);
            xi_lower_braun_seal(l, value_blk);
            xi_lower_braun_seal(l, fallback_blk);

            xi_block_set_jump(value_blk, merge);
            xi_block_set_jump(fallback_blk, merge);

            xi_lower_braun_seal(l, merge);
            l->cur_block = merge;
            XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
            if (phi) {
                for (uint16_t i = 0; i < merge->npreds; i++) {
                    if (merge->preds[i] == value_blk)
                        phi->value.args[i] = chan_recv;
                    else
                        phi->value.args[i] = arg_vals[0];
                }
            }
            return phi ? &phi->value : chan_recv;
        }

        XiCallWriteback *writebacks = NULL;
        XiCallPlan *call_plan = lower_build_call_plan(
            l, call, arg_vals, n, method_modes, method_pcount, method_read_places,
            method_read_place_count, method_type, &writebacks, (int) node->line);
        if (l->had_error)
            return NULL;

        XiCallWriteback receiver_writeback;
        memset(&receiver_writeback, 0, sizeof(receiver_writeback));
        XrParamMode receiver_mode = XR_PARAM_READ;
        const XaSelection *method_selection = xa_analyzer_get_selection(l->analyzer, call->callee);
        if (lower_method_receiver_mode(l, method_selection, recv, &receiver_mode)) {
            XiValue *receiver_place = lower_build_method_receiver_place(
                l, call, ma->object, recv, n, receiver_mode, &call_plan, &receiver_writeback,
                (int) node->line);
            if (!receiver_place)
                return NULL;
            recv = receiver_place;
        }

        uint16_t nargs = (uint16_t) (n + 1); /* receiver + args */
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
        if (!v)
            return NULL;
        v->args[0] = recv;
        for (int i = 0; i < n; i++)
            v->args[i + 1] = arg_vals[i];
        v->aux = (void *) arena_strdup(l->func, ma->name);
        v->aux_int = (int64_t) xi_lower_method_symbol(l, ma->name) << 1;
        v->call_plan = call_plan;
        if (is_time_sleep)
            v->lowering_flags |= XI_LOWERING_FLAG_TIME_SLEEP;
        lower_instantiate_call_view_evidence(v, NULL, method_type, true);
        v->flags |= XI_FLAG_SIDE_EFFECT;
        if (xi_lower_method_may_suspend(recv->type, ma->name, n))
            v->flags |= XI_FLAG_MAY_SUSPEND;
        v->line = (uint32_t) node->line;
        xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
        xi_lower_bind_callsite_id(l, v, xi_lower_source_node_id(l, node));
        if (json_codec_kind != 0)
            xi_lower_bind_json_codec_id(l, v, xi_lower_source_node_id(l, node), json_codec_kind);
        xi_lower_bind_key_access_id(l, v, (uint32_t) node->line, method_key_access_ordinal,
                                    method_key_access_op);

        lower_call_emit_err_check(l, v, node, call, method_type);
        if (!lower_apply_call_writebacks(l, call_plan, writebacks, (int) node->line))
            return NULL;
        if (!lower_apply_one_call_writeback(l, &receiver_writeback, (int) node->line))
            return NULL;
        return v;
    }

    /* Optional chain method call: obj?.method(args) — null short-circuit
     * with XI_CALL_METHOD on the non-null path. chain_type==2 signals
     * the parser detected a call immediately after the optional chain. */
    if (call->callee && call->callee->type == AST_OPTIONAL_CHAIN &&
        call->callee->as.optional_chain.name && call->callee->as.optional_chain.chain_type == 2) {
        OptionalChainNode *oc = &call->callee->as.optional_chain;
        XiValue *obj = xi_lower_expr(l, oc->object);
        if (!obj)
            return NULL;

        XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
        if (!is_null)
            return obj;
        is_null->args[0] = obj;

        XiBlock *call_blk = xi_block_new(l->func);
        XiBlock *null_blk = xi_block_new(l->func);
        XiBlock *merge = xi_block_new(l->func);

        xi_block_set_if(l->cur_block, is_null, null_blk, call_blk);
        xi_lower_braun_seal(l, call_blk);
        xi_lower_braun_seal(l, null_blk);

        /* Null path */
        l->cur_block = null_blk;
        struct XrType *result_type = xi_lower_node_type(l, node);
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        xi_block_set_jump(l->cur_block, merge);

        /* Non-null path: emit XI_CALL_METHOD */
        l->cur_block = call_blk;
        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerArgList args;
        xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, NULL, 0, NULL, 0,
                                           (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;

        xi_lower_check_map_method_args(l, node, oc->name, obj, arg_vals, n);
        xi_lower_check_set_method_args(l, node, oc->name, obj, arg_vals, n);
        xi_lower_narrow_map_method_args(l, node, oc->name, obj, arg_vals, n);
        xi_lower_narrow_set_method_args(l, node, oc->name, obj, arg_vals, n);

        uint16_t nargs = (uint16_t) (n + 1);
        XiValue *mcall = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
        if (!mcall)
            return NULL;
        mcall->args[0] = obj;
        for (int i = 0; i < n; i++)
            mcall->args[i + 1] = arg_vals[i];
        mcall->aux = (void *) arena_strdup(l->func, oc->name);
        mcall->aux_int = (int64_t) xi_lower_method_symbol(l, oc->name) << 1;
        XrType *optional_method_type =
            xr_type_non_nullable(l->isolate, xi_lower_node_type(l, call->callee));
        lower_instantiate_call_view_evidence(mcall, NULL, optional_method_type, true);
        mcall->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        mcall->line = (uint32_t) node->line;
        xi_lower_bind_callsite_id(l, mcall, xi_lower_source_node_id(l, node));
        XiBlock *call_exit = l->cur_block;
        xi_block_set_jump(call_exit, merge);

        /* Merge: PHI(null, method_result) */
        xi_lower_braun_seal(l, merge);
        l->cur_block = merge;
        XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
        if (phi) {
            for (uint16_t i = 0; i < merge->npreds; i++) {
                if (merge->preds[i] == null_blk)
                    phi->value.args[i] = null_val;
                else
                    phi->value.args[i] = mcall;
            }
        }
        return phi ? &phi->value : null_val;
    }

    /* Optional function call: func?.(args) — evaluate args only on the non-null
     * path and lower to a normal XI_CALL there. */
    if (call->callee && call->callee->type == AST_OPTIONAL_CHAIN &&
        call->callee->as.optional_chain.chain_type == 3) {
        OptionalChainNode *oc = &call->callee->as.optional_chain;
        XiValue *callee_val = xi_lower_expr(l, oc->object);
        if (!callee_val)
            return NULL;

        XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
        if (!is_null)
            return callee_val;
        is_null->args[0] = callee_val;

        XiBlock *call_blk = xi_block_new(l->func);
        XiBlock *null_blk = xi_block_new(l->func);
        XiBlock *merge = xi_block_new(l->func);

        xi_block_set_if(l->cur_block, is_null, null_blk, call_blk);
        xi_lower_braun_seal(l, call_blk);
        xi_lower_braun_seal(l, null_blk);

        l->cur_block = null_blk;
        struct XrType *result_type = xi_lower_node_type(l, node);
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        xi_block_set_jump(l->cur_block, merge);

        l->cur_block = call_blk;
        XiValue *call_val =
            lower_emit_function_call(l, node, call, callee_val, xi_lower_node_type(l, oc->object));
        if (!call_val)
            return NULL;
        XiBlock *call_exit = l->cur_block;
        xi_block_set_jump(call_exit, merge);

        xi_lower_braun_seal(l, merge);
        l->cur_block = merge;
        XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
        if (phi) {
            for (uint16_t i = 0; i < merge->npreds; i++) {
                if (merge->preds[i] == null_blk)
                    phi->value.args[i] = null_val;
                else
                    phi->value.args[i] = call_val;
            }
        }
        return phi ? &phi->value : null_val;
    }

    /* Compile-time builtin interception: detect calls to known builtins
     * and emit specialized Xi ops instead of generic XI_CALL. */
    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *fname = call->callee->as.variable.name;
        const XaParallelCallPlan *analyzer_parallel_plan =
            lower_analyzer_parallel_call_plan(l, node);
        if (analyzer_parallel_plan && !analyzer_parallel_plan->is_plan_method) {
            XiValue *parallel_intrinsic = lower_parallel_module_intrinsic_or_error(
                l, node, call, analyzer_parallel_plan->kind, analyzer_parallel_plan->intrinsic_id);
            if (parallel_intrinsic || l->had_error)
                return parallel_intrinsic;
        }
        XiValue *bi = lower_builtin_call(l, node, fname, call);
        if (bi)
            return bi;
    }

    /* `T(args)` where T is a class name constructs an instance (no `new`).
     * Unified with new-expr so nested/builtin/Exception classes all work. */
    XiValue *constructed = lower_try_construct_call(l, node, call);
    if (constructed)
        return constructed;

    /* Evaluate callee and all arguments before creating CALL */
    XiValue *callee_val = xi_lower_expr(l, call->callee);
    if (!callee_val)
        return NULL;

    return lower_emit_function_call(l, node, call, callee_val, xi_lower_node_type(l, call->callee));
}

static XiValue *xi_lower_narrow_select_arm(XiLower *l, AstNode *node, XiValue *val,
                                           struct XrType *result_type) {
    if (!val || !result_type || !val->type || xr_type_assignable(result_type, val->type))
        return val;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (!copy)
        return val;
    copy->args[0] = val;
    copy->line = (uint32_t) node->line;
    return copy;
}

static XiValue *lower_ternary(XiLower *l, AstNode *node) {
    XiValue *cond = xi_lower_expr(l, node->as.ternary.condition);
    if (!cond)
        return NULL;
    cond = xi_lower_bool_condition(l, cond);

    struct XrType *result_type = xi_lower_node_type(l, node);

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *else_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);
    xi_lower_braun_seal(l, then_blk);
    xi_lower_braun_seal(l, else_blk);

    l->cur_block = then_blk;
    XiValue *then_val = xi_lower_expr(l, node->as.ternary.true_expr);
    then_val = xi_lower_narrow_select_arm(l, node, then_val, result_type);
    XiBlock *then_exit = l->cur_block;
    xi_block_set_jump(then_exit, merge);

    l->cur_block = else_blk;
    XiValue *else_val = xi_lower_expr(l, node->as.ternary.false_expr);
    else_val = xi_lower_narrow_select_arm(l, node, else_val, result_type);
    XiBlock *else_exit = l->cur_block;
    xi_block_set_jump(else_exit, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;
    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == then_exit)
                phi->value.args[i] = then_val;
            else
                phi->value.args[i] = else_val;
        }
    }
    return phi ? &phi->value : then_val;
}

/*
 * Nullish coalesce (a ?? b): if a is null, evaluate b; otherwise use a.
 * Similar to short-circuit OR but checks null instead of falsy.
 */
static XiValue *lower_nullish_coalesce(XiLower *l, AstNode *node) {
    /* Partially dead after canonicalization: simple LHS is canonicalized
     * to ternary, but complex LHS still falls through to here. */
    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    if (!lhs)
        return NULL;

    XiBlock *eval_rhs = xi_block_new(l->func);
    XiBlock *skip = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    /* Test: is lhs null? */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null)
        return lhs;
    is_null->args[0] = lhs;

    /* If null → eval rhs; otherwise → skip (use lhs) */
    xi_block_set_if(l->cur_block, is_null, eval_rhs, skip);
    xi_lower_braun_seal(l, eval_rhs);
    xi_lower_braun_seal(l, skip);

    struct XrType *result_type = xi_lower_node_type(l, node);

    /* Evaluate RHS in eval_rhs block */
    l->cur_block = eval_rhs;
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip → merge (lhs is non-null) */
    l->cur_block = skip;
    XiValue *skip_val = xi_lower_narrow_select_arm(l, node, lhs, result_type);
    xi_block_set_jump(skip, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == rhs_exit)
                phi->value.args[i] = rhs ? rhs : lhs;
            else
                phi->value.args[i] = skip_val;
        }
    }
    return phi ? &phi->value : lhs;
}

static XiValue *lower_map_literal(XiLower *l, AstNode *node) {
    MapLiteralNode *map = &node->as.map_literal;
    int count = map->count;

    /* Evaluate all keys and values first */
    int n = count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **key_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    XiValue **val_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!key_vals || !val_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        key_vals[i] = xi_lower_expr(l, map->keys[i]);
        val_vals[i] = xi_lower_expr(l, map->values[i]);
        if (!key_vals[i] || !val_vals[i])
            return NULL;
    }

    /* Create map: XI_MAP_NEW with capacity */
    struct XrType *result_type = xi_lower_node_type(l, node);
    if (result_type && XR_TYPE_IS_MAP(result_type)) {
        for (int i = 0; i < n; i++) {
            key_vals[i] = xi_lower_apply_numeric_conversion_witness(l, map->keys[i], key_vals[i],
                                                                    result_type->map.key_type);
            val_vals[i] = xi_lower_apply_numeric_conversion_witness(l, map->values[i], val_vals[i],
                                                                    result_type->map.value_type);
            if (!key_vals[i] || !val_vals[i])
                return NULL;
            key_vals[i] = xi_lower_narrow_for_static_type(l, map->keys[i], key_vals[i],
                                                          result_type->map.key_type);
            val_vals[i] = xi_lower_narrow_for_static_type(l, map->values[i], val_vals[i],
                                                          result_type->map.value_type);
        }
    }
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *map_val = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
    if (!map_val)
        return NULL;
    map_val->args[0] = cap;
    map_val->line = (uint32_t) node->line;
    xi_lower_bind_map_shape_id(l, map_val, (uint32_t) node->line, XG_MAP_CONTAINER_MAP);
    if (map_val->xg_map_shape_id != XG_NO_ID) {
        XiMapLiteralData *data =
            (XiMapLiteralData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiMapLiteralData));
        if (!data)
            return NULL;
        data->keys = key_vals;
        data->values = val_vals;
        data->count = (uint16_t) count;
        data->container_kind = XG_MAP_CONTAINER_MAP;
        map_val->aux = data;
        map_val->aux_kind = XI_AUX_KIND_MAP_LITERAL;
    }

    /* Populate: INDEX_SET for each key-value pair */
    for (int i = 0; i < n; i++) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
        if (!set)
            break;
        set->args[0] = map_val;
        set->args[1] = key_vals[i];
        set->args[2] = val_vals[i];
        set->flags |= XI_FLAG_SIDE_EFFECT;
        set->xg_map_shape_id = map_val->xg_map_shape_id;
    }
    return map_val;
}

XR_FUNC void xi_lower_func_add_child(XiFunc *parent, XiFunc *child) {
    if (parent->nchildren >= parent->children_cap) {
        uint16_t new_cap = parent->children_cap ? parent->children_cap * 2 : 4;
        XiFunc **tmp = (XiFunc **) xr_realloc(parent->children, new_cap * sizeof(XiFunc *));
        if (!tmp)
            return;
        parent->children = tmp;
        parent->children_cap = new_cap;
    }
    parent->children[parent->nchildren++] = child;
}

/*
 * Lower a function declaration / function expression.
 * Recursively lowers the function body into a child XiFunc,
 * then emits XI_CLOSURE_NEW in the parent to produce a callable value.
 */
/* xi_lower_func_impl declared in xi_lower_internal.h */

XR_FUNC XiValue *xi_lower_function_decl(XiLower *l, AstNode *node) {
    /* Recursively lower the function body into a child XiFunc,
     * passing 'l' as parent so the child can resolve upvalue captures. */
    XiFunc *child = xi_lower_func_impl(node, l->analyzer, l->isolate, l, l->typed_program);
    if (!child) {
        l->had_error = true;
        return NULL;
    }

    /* Register as child of parent function */
    xi_lower_func_add_child(l->func, child);
    uint16_t child_idx = (uint16_t) (l->func->nchildren - 1);

    /* Emit CLOSURE_NEW with captured values as args.  Listing them as
     * args ensures liveness analysis keeps their registers alive until
     * the closure instruction executes (prevents premature recycling). */
    struct XrType *fn_type = xi_lower_node_type(l, node);
    uint16_t ncap = child->ncaptures;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, fn_type, ncap);
    if (!v)
        return NULL;
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &child->captures[ci];
        v->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value) ? cap->value : NULL;
    }
    v->aux = (void *) child;
    v->aux_int = child_idx;
    v->line = (uint32_t) node->line;

    /* If named, register in SSA so the function can be called by name */
    FunctionDeclNode *fdecl = &node->as.function_decl;
    if (fdecl->name) {
        int var_id = xi_lower_var_create(l, fdecl->symbol_id, fdecl->name, fn_type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Hoisted closures must survive DCE: they are stored into cells
         * at emit time for mutable upvalue capture by sibling functions. */
        if (var_id >= 0 && var_id < l->var_count && l->vars[var_id].hoisted)
            v->flags |= XI_FLAG_SIDE_EFFECT;

        /* For program-level named functions, also store into backing
         * store so nested functions can access (forward refs). */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            int slot = l->shared_map[var_id];
            XiTopBinding b;
            b.slot = slot;
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, v);
            /* Track function → shared slot for module export metadata */
            if (slot >= 0 && slot < l->var_cap) {
                l->shared_slot_funcs[slot] = child;
                if (l->func->shared_slot_funcs && slot < (int) l->func->shared_slot_func_count)
                    l->func->shared_slot_funcs[slot] = child;
            }
        }
    }

    return v;
}

static AstNode *parallel_call_unwrap_grouping(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node;
}

static bool parallel_call_int_literal_value(AstNode *node, int64_t *out) {
    node = parallel_call_unwrap_grouping(node);
    if (!node)
        return false;
    if (node->type == AST_LITERAL_INT) {
        if (out)
            *out = node->as.literal.raw_value.int_val;
        return true;
    }
    if (node->type == AST_UNARY_NEG) {
        AstNode *operand = parallel_call_unwrap_grouping(node->as.unary.operand);
        if (operand && operand->type == AST_LITERAL_INT) {
            if (out)
                *out = -operand->as.literal.raw_value.int_val;
            return true;
        }
    }
    return false;
}

static bool parallel_call_options_ctor_is_parallel(XiLower *l, AstNode *callee) {
    callee = parallel_call_unwrap_grouping(callee);
    if (!callee)
        return false;
    if (callee->type == AST_VARIABLE) {
        const char *member = lower_call_callee_imported_member(l, callee, "parallel");
        return member && strcmp(member, "Options") == 0;
    }
    if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &callee->as.member_access;
        return ma->name && strcmp(ma->name, "Options") == 0 &&
               lower_call_object_is_module(l, ma->object, "parallel");
    }
    return false;
}

static AstNode *parallel_call_options_workers_ast(XiLower *l, AstNode *options_arg,
                                                  bool *out_supported) {
    if (out_supported)
        *out_supported = false;
    options_arg = parallel_call_unwrap_grouping(options_arg);
    if (!options_arg || options_arg->type != AST_CALL_EXPR)
        return NULL;
    CallExprNode *ctor = &options_arg->as.call_expr;
    if (!parallel_call_options_ctor_is_parallel(l, ctor->callee))
        return NULL;
    if (ctor->arg_count == 0) {
        if (out_supported)
            *out_supported = true;
        return NULL;
    }
    if (ctor->arg_count != 1)
        return NULL;
    int64_t literal = 0;
    if (!parallel_call_int_literal_value(ctor->arguments[0], &literal))
        return NULL;
    if (out_supported)
        *out_supported = true;
    return ctor->arguments[0];
}

static bool parallel_call_extract_workers(XiLower *l, CallExprNode *call, int options_index,
                                          XiValue **out_workers) {
    if (!out_workers)
        return false;
    *out_workers = NULL;
    if (options_index < 0 || options_index >= call->arg_count) {
        *out_workers = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        return *out_workers != NULL;
    }

    bool supported = false;
    AstNode *workers_ast =
        parallel_call_options_workers_ast(l, call->arguments[options_index], &supported);
    if (!supported)
        return false;
    if (!workers_ast) {
        *out_workers = xi_const_int(l->func, l->cur_block, 0, l->type_int);
        return *out_workers != NULL;
    }
    *out_workers = xi_lower_expr(l, workers_ast);
    return *out_workers != NULL;
}

static bool parallel_call_is_plain_lambda(AstNode *node, int param_count) {
    node = parallel_call_unwrap_grouping(node);
    return node && node->type == AST_FUNCTION_EXPR &&
           node->as.function_expr.param_count == param_count && node->as.function_expr.body != NULL;
}

static struct XrType *parallel_call_param_type(XiLower *l, XrParamNode *param,
                                               struct XrType *fallback) {
    if (l && l->analyzer && param && param->symbol_id != 0) {
        XaSymbol *sym = xa_scope_lookup_by_id(l->analyzer->global_scope, param->symbol_id);
        XaSymbolLinks *links = sym ? xa_analyzer_get_links(l->analyzer, sym) : NULL;
        if (links && links->type)
            return xi_lower_type_or_any(l, links->type, "parallel lambda parameter type",
                                        param->line);
    }
    struct XrType *type = (param && param->type) ? xr_tref_resolve(l->isolate, param->type) : NULL;
    return type ? xi_lower_type_or_any(l, type, "parallel lambda parameter type", param->line)
                : fallback;
}

typedef struct ParallelCallParamBinding {
    XrParamNode *param;
    uint16_t abi_index;
    struct XrType *type;
} ParallelCallParamBinding;

static XiFunc *parallel_call_lower_lambda_func(
    XiLower *parent, AstNode *lambda_node, const char *suffix, struct XrType *return_type,
    XiNativeCallbackKind callback_kind, uint16_t abi_param_count, struct XrType **abi_param_types,
    ParallelCallParamBinding *bindings, uint16_t binding_count, int line) {
    lambda_node = parallel_call_unwrap_grouping(lambda_node);
    if (!parent || !lambda_node || lambda_node->type != AST_FUNCTION_EXPR || !suffix ||
        abi_param_count == 0)
        return NULL;
    FunctionDeclNode *fn = &lambda_node->as.function_expr;

    char name_buf[160];
    snprintf(name_buf, sizeof(name_buf), "%s$parallel_%s_%d",
             parent->func && parent->func->name ? parent->func->name : "<anon>", suffix,
             parent->synthetic_id++);

    XiLower child_l;
    xi_lower_init(&child_l, parent->analyzer, parent->isolate);
    child_l.parent = parent;
    child_l.repl_mode = parent->repl_mode;
    xi_lower_inherit_evidence(&child_l, parent);

    child_l.func = xi_func_new(name_buf, return_type ? return_type : child_l.type_unit);
    if (!child_l.func) {
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->native_callback_kind = callback_kind;
    child_l.func->parent_func = parent->func;
    child_l.func->analyzer = parent->analyzer;
    child_l.func->is_generic_template = parent->func && parent->func->is_generic_template;
    XaScope *semantic_scope = xa_scope_find_by_node(parent->analyzer->global_scope, lambda_node);
    if (semantic_scope && semantic_scope->return_storage_known &&
        !semantic_scope->return_storage_mixed) {
        child_l.func->return_storage_domain = semantic_scope->return_storage_domain;
        child_l.func->return_storage_known = true;
    }
    xi_lower_bind_function_body_id(&child_l, xi_lower_source_node_id(&child_l, lambda_node),
                                   lambda_node->line > 0 ? (uint32_t) lambda_node->line : 0);
    child_l.func->nparams = abi_param_count;
    child_l.func->min_params = abi_param_count;
    child_l.func->entry_type = 0;
    child_l.func->params = (XiValue **) xr_calloc(abi_param_count, sizeof(XiValue *));
    if (!child_l.func->params) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiBlock *entry = xi_block_new(child_l.func);
    if (!entry) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    entry->sealed = true;
    child_l.cur_block = entry;

    for (uint16_t i = 0; i < abi_param_count; i++) {
        struct XrType *ptype =
            abi_param_types && abi_param_types[i] ? abi_param_types[i] : child_l.type_any;
        XiValue *param = xi_param(child_l.func, entry, i, ptype);
        if (!param) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        child_l.func->params[i] = param;
    }

    for (uint16_t i = 0; i < binding_count; i++) {
        ParallelCallParamBinding *binding = &bindings[i];
        if (!binding->param || binding->abi_index >= abi_param_count || !binding->param->name) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        struct XrType *ptype = parallel_call_param_type(
            &child_l, binding->param,
            binding->type ? binding->type : child_l.func->params[binding->abi_index]->type);
        int var_id =
            xi_lower_var_create(&child_l, binding->param->symbol_id, binding->param->name, ptype);
        if (var_id < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        xi_lower_braun_write(&child_l, var_id, entry, child_l.func->params[binding->abi_index]);
    }

    xi_lower_defer_scope_push(&child_l);
    if (fn->body)
        xi_lower_stmt(&child_l, fn->body);
    xi_lower_defer_scope_pop_normal(&child_l, line);

    if (child_l.cur_block)
        xi_block_set_return(child_l.cur_block, NULL);

    XiFunc *result = NULL;
    if (!child_l.had_error && xi_lower_capture_source_vars(&child_l)) {
        result = child_l.func;
    } else {
        xi_func_free(child_l.func);
    }
    xi_lower_cleanup(&child_l);
    return result;
}

static XiValue *parallel_call_child_closure(XiLower *l, XiFunc *child, int line) {
    if (!l || !child)
        return NULL;
    uint16_t before = l->func->nchildren;
    xi_lower_func_add_child(l->func, child);
    if (l->func->nchildren == before) {
        xi_func_free(child);
        l->had_error = true;
        return NULL;
    }
    uint16_t child_idx = (uint16_t) (l->func->nchildren - 1);
    uint16_t ncap = child->ncaptures;
    XiValue *closure = xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, l->type_any, ncap);
    if (!closure) {
        l->had_error = true;
        return NULL;
    }
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &child->captures[ci];
        closure->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value) ? cap->value : NULL;
    }
    closure->aux = (void *) child;
    closure->aux_int = child_idx;
    closure->line = (uint32_t) line;
    return closure;
}

static bool parallel_call_type_can_use_scalar_map_callback(const struct XrType *type) {
    return type && !type->is_nullable &&
           (XR_TYPE_IS_INT(type) || XR_TYPE_IS_FLOAT(type) || XR_TYPE_IS_BOOL(type) ||
            XR_TYPE_IS_RUNE(type));
}

static XiValue *parallel_call_make_for_each(XiLower *l, AstNode *node, AstNode *range,
                                            AstNode *body_node, XiValue *workers) {
    RangeNode *rn = &range->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    if (!start || !end || !workers)
        return NULL;

    FunctionDeclNode *body_expr = &parallel_call_unwrap_grouping(body_node)->as.function_expr;
    struct XrType *abi_types[2] = {l->type_int, l->type_int};
    ParallelCallParamBinding bindings[1] = {{
        .param = body_expr->params[0],
        .abi_index = 0,
        .type = l->type_int,
    }};
    XiFunc *body = parallel_call_lower_lambda_func(l, body_node, "for_each_body", l->type_unit,
                                                   XI_NATIVE_CALLBACK_PAR_FOR_I64, 2, abi_types,
                                                   bindings, 1, node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = parallel_call_child_closure(l, body, node->line);
    if (!closure)
        return NULL;
    uint16_t ncap = body->ncaptures;

    XiParallelForData *data =
        (XiParallelForData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiParallelForData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = body;
    data->item_name =
        body_expr->params[0] ? arena_strdup(l->func, body_expr->params[0]->name) : NULL;
    data->item_symbol_id = body_expr->params[0] ? body_expr->params[0]->symbol_id : 0;
    data->body_child_index = (uint16_t) closure->aux_int;
    data->inclusive_end = rn->inclusive_end;
    data->range_body = false;

    XiValue *par =
        xi_value_new(l->func, l->cur_block, XI_PAR_FOR, l->type_unit, (uint16_t) (4u + ncap));
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = closure;
    for (uint16_t ci = 0; ci < ncap; ci++)
        par->args[4 + ci] = closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_FOR;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                  XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) node->line;
    xi_lower_insert_err_check(l, node, true);
    return par;
}

static XiValue *parallel_call_make_map(XiLower *l, AstNode *node, AstNode *range,
                                       AstNode *body_node, XiValue *workers, XiValue *into_array) {
    RangeNode *rn = &range->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    if (!start || !end || !workers)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *elem_type = NULL;
    if (into_array && into_array->type && XR_TYPE_IS_ARRAY(into_array->type))
        elem_type = xi_get_container_elem_type(into_array->type);
    if (!elem_type && result_type && XR_TYPE_IS_ARRAY(result_type))
        elem_type = xi_get_container_elem_type(result_type);
    if (!elem_type)
        elem_type = l->type_any;
    if (!result_type || !XR_TYPE_IS_ARRAY(result_type))
        result_type = into_array && into_array->type ? into_array->type
                                                     : xr_type_new_array(l->isolate, elem_type);

    FunctionDeclNode *body_expr = &parallel_call_unwrap_grouping(body_node)->as.function_expr;
    struct XrType *abi_types[2] = {l->type_int, l->type_int};
    ParallelCallParamBinding bindings[1] = {{
        .param = body_expr->params[0],
        .abi_index = 0,
        .type = l->type_int,
    }};
    XiNativeCallbackKind callback = parallel_call_type_can_use_scalar_map_callback(elem_type)
                                        ? XI_NATIVE_CALLBACK_PAR_MAP_SCALAR_BODY
                                        : XI_NATIVE_CALLBACK_NONE;
    XiFunc *body = parallel_call_lower_lambda_func(l, body_node, "map_body", elem_type, callback, 2,
                                                   abi_types, bindings, 1, node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = parallel_call_child_closure(l, body, node->line);
    if (!closure)
        return NULL;
    uint16_t ncap = body->ncaptures;
    uint16_t extra_count = into_array ? 1u : 0u;
    uint16_t capture_base = (uint16_t) (4u + extra_count);

    XiParallelMapData *data =
        (XiParallelMapData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiParallelMapData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = body;
    data->element_type = elem_type;
    data->item_name =
        body_expr->params[0] ? arena_strdup(l->func, body_expr->params[0]->name) : NULL;
    data->item_symbol_id = body_expr->params[0] ? body_expr->params[0]->symbol_id : 0;
    data->body_child_index = (uint16_t) closure->aux_int;
    data->result_capture_index = ncap;
    data->start_capture_index = (uint16_t) (ncap + 1u);
    data->lane_count = 1;
    data->inclusive_end = rn->inclusive_end;
    data->into_result = into_array != NULL;

    XiValue *par = xi_value_new(l->func, l->cur_block, XI_PAR_MAP, result_type,
                                (uint16_t) (capture_base + ncap));
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = closure;
    if (into_array)
        par->args[4] = into_array;
    for (uint16_t ci = 0; ci < ncap; ci++)
        par->args[capture_base + ci] = closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_MAP;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                  XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) node->line;
    xi_lower_insert_err_check(l, node, true);

    if (!into_array)
        return par;
    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *parallel_call_make_reduce(XiLower *l, AstNode *node, AstNode *range,
                                          AstNode *initial_node, AstNode *body_node,
                                          AstNode *combine_node, XiValue *workers) {
    RangeNode *rn = &range->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    XiValue *initial = xi_lower_expr(l, initial_node);
    if (!start || !end || !workers || !initial)
        return NULL;

    struct XrType *acc_type = xi_lower_node_type(l, node);
    if (!acc_type || XR_TYPE_IS_UNKNOWN(acc_type))
        acc_type = initial->type ? initial->type : l->type_int;
    initial = xi_lower_checktype_for_type(l, initial_node, initial, acc_type);
    bool native_i64 = acc_type && XR_TYPE_IS_INT(acc_type);
    bool native_agg = !native_i64 && xi_lower_type_struct_layout(l, acc_type) != NULL;
    XiNativeCallbackKind body_callback = XI_NATIVE_CALLBACK_NONE;
    XiNativeCallbackKind combine_callback = XI_NATIVE_CALLBACK_NONE;
    if (native_i64) {
        body_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY;
        combine_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE;
    } else if (native_agg) {
        body_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_BODY;
        combine_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE;
    }

    FunctionDeclNode *body_expr = &parallel_call_unwrap_grouping(body_node)->as.function_expr;
    struct XrType *body_abi_types[2] = {l->type_int, l->type_int};
    ParallelCallParamBinding body_bindings[1] = {{
        .param = body_expr->params[0],
        .abi_index = 0,
        .type = l->type_int,
    }};
    XiFunc *body =
        parallel_call_lower_lambda_func(l, body_node, "reduce_body", acc_type, body_callback, 2,
                                        body_abi_types, body_bindings, 1, node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }

    FunctionDeclNode *combine_expr = &parallel_call_unwrap_grouping(combine_node)->as.function_expr;
    struct XrType *combine_abi_types[2] = {acc_type, acc_type};
    ParallelCallParamBinding combine_bindings[2] = {{
                                                        .param = combine_expr->params[0],
                                                        .abi_index = 0,
                                                        .type = acc_type,
                                                    },
                                                    {
                                                        .param = combine_expr->params[1],
                                                        .abi_index = 1,
                                                        .type = acc_type,
                                                    }};
    XiFunc *combine = parallel_call_lower_lambda_func(l, combine_node, "reduce_combine", acc_type,
                                                      combine_callback, 2, combine_abi_types,
                                                      combine_bindings, 2, node->line);
    if (!combine) {
        xi_func_free(body);
        l->had_error = true;
        return NULL;
    }

    XiValue *body_closure = parallel_call_child_closure(l, body, node->line);
    if (!body_closure) {
        xi_func_free(combine);
        return NULL;
    }
    XiValue *combine_closure = parallel_call_child_closure(l, combine, node->line);
    if (!combine_closure)
        return NULL;
    uint16_t body_ncap = body->ncaptures;
    uint16_t combine_ncap = combine->ncaptures;

    XiParallelReduceData *data = (XiParallelReduceData *) xi_func_arena_alloc(
        l->func, (uint32_t) sizeof(XiParallelReduceData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = body;
    data->combine_func = combine;
    data->accumulator_type = acc_type;
    data->item_name =
        body_expr->params[0] ? arena_strdup(l->func, body_expr->params[0]->name) : NULL;
    data->item_symbol_id = body_expr->params[0] ? body_expr->params[0]->symbol_id : 0;
    data->body_child_index = (uint16_t) body_closure->aux_int;
    data->combine_child_index = (uint16_t) combine_closure->aux_int;
    data->inclusive_end = rn->inclusive_end;
    data->range_body = false;

    uint16_t nargs = (uint16_t) (6u + body_ncap + combine_ncap);
    XiValue *par = xi_value_new(l->func, l->cur_block, XI_PAR_REDUCE, acc_type, nargs);
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = initial;
    par->args[4] = body_closure;
    par->args[5] = combine_closure;
    for (uint16_t ci = 0; ci < body_ncap; ci++)
        par->args[6 + ci] = body_closure->args[ci];
    for (uint16_t ci = 0; ci < combine_ncap; ci++)
        par->args[6 + body_ncap + ci] = combine_closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_REDUCE;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                  XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) node->line;
    xi_lower_insert_err_check(l, node, true);
    return par;
}

static XiValue *lower_parallel_module_intrinsic_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                     XaParallelCallKind kind) {
    if (!l || !node || !call || kind == XA_PAR_CALL_NONE)
        return NULL;

    if (kind == XA_PAR_CALL_FOR_EACH) {
        if (call->arg_count != 2 && call->arg_count != 3)
            return NULL;
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[1], 1))
            return NULL;
        XiValue *workers = NULL;
        if (!parallel_call_extract_workers(l, call, call->arg_count == 3 ? 2 : -1, &workers))
            return NULL;
        return parallel_call_make_for_each(l, node, range, call->arguments[1], workers);
    }

    if (kind == XA_PAR_CALL_MAP) {
        if (call->arg_count != 2 && call->arg_count != 3)
            return NULL;
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[1], 1))
            return NULL;
        XiValue *workers = NULL;
        if (!parallel_call_extract_workers(l, call, call->arg_count == 3 ? 2 : -1, &workers))
            return NULL;
        return parallel_call_make_map(l, node, range, call->arguments[1], workers, NULL);
    }

    if (kind == XA_PAR_CALL_MAP_INTO) {
        if (call->arg_count != 3 && call->arg_count != 4)
            return NULL;
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[2], 1))
            return NULL;
        XiValue *workers = NULL;
        if (!parallel_call_extract_workers(l, call, call->arg_count == 4 ? 3 : -1, &workers))
            return NULL;
        XiValue *into = xi_lower_expr(l, call->arguments[1]);
        if (!into)
            return NULL;
        return parallel_call_make_map(l, node, range, call->arguments[2], workers, into);
    }

    if (kind == XA_PAR_CALL_REDUCE) {
        if (call->arg_count != 4 && call->arg_count != 5)
            return NULL;
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[2], 1) ||
            !parallel_call_is_plain_lambda(call->arguments[3], 2))
            return NULL;
        XiValue *workers = NULL;
        if (!parallel_call_extract_workers(l, call, call->arg_count == 5 ? 4 : -1, &workers))
            return NULL;
        return parallel_call_make_reduce(l, node, range, call->arguments[1], call->arguments[2],
                                         call->arguments[3], workers);
    }

    return NULL;
}

static struct XrType *lower_parallel_plan_state_type(XiLower *l, XrType *plan_type) {
    if (plan_type && XR_TYPE_IS_INSTANCE(plan_type) && plan_type->instance.type_arg_count > 0 &&
        plan_type->instance.type_args && plan_type->instance.type_args[0])
        return plan_type->instance.type_args[0];
    return l ? l->type_any : NULL;
}

static XiValue *lower_parallel_plan_lifecycle_call(XiLower *l, AstNode *node, XiValue *plan,
                                                   const char *method) {
    if (!l || !plan || !method)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_unit, 1);
    if (!v)
        return NULL;
    v->args[0] = plan;
    v->aux = (void *) arena_strdup(l->func, method);
    v->aux_int = (int64_t) xi_lower_method_symbol(l, method) << 1;
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    v->lowering_flags |= XI_LOWERING_FLAG_PARALLEL_PLAN_LIFECYCLE;
    v->line = (uint32_t) (node ? node->line : 0);
    xi_lower_insert_err_check(l, node, true);
    return v;
}

static XiValue *lower_parallel_plan_end_defer_closure(XiLower *l, AstNode *node, XiValue *plan) {
    if (!l || !l->func || !l->cur_block || !plan)
        return NULL;

    char name_buf[160];
    snprintf(name_buf, sizeof(name_buf), "%s$parallel_plan_end_defer_%d",
             l->func && l->func->name ? l->func->name : "<anon>", l->synthetic_id++);

    XiLower child_l;
    xi_lower_init(&child_l, l->analyzer, l->isolate);
    child_l.parent = l;
    child_l.repl_mode = l->repl_mode;
    xi_lower_inherit_evidence(&child_l, l);

    child_l.func = xi_func_new(name_buf, child_l.type_unit);
    if (!child_l.func) {
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->parent_func = l->func;
    child_l.func->analyzer = l->analyzer;
    child_l.func->is_generic_template = l->func && l->func->is_generic_template;
    xi_lower_bind_function_body_id(&child_l, node ? xi_lower_source_node_id(&child_l, node) : 0,
                                   node && node->line > 0 ? (uint32_t) node->line : 0);
    child_l.func->nparams = 0;
    child_l.func->min_params = 0;
    child_l.func->entry_type = 0;

    XiBlock *entry = xi_block_new(child_l.func);
    if (!entry) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    entry->sealed = true;
    child_l.cur_block = entry;

    XiCapture *cap = &child_l.func->captures[0];
    if (xi_lower_reject_error_type(&child_l, plan->type, "capture metadata",
                                   node ? node->line : 0)) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    cap->source = XI_CAPTURE_SRC_REG;
    cap->index = 0;
    cap->name = arena_strdup(child_l.func, "__parallel_plan");
    cap->type = plan->type ? plan->type : child_l.type_any;
    cap->value = plan;
    cap->cell_index = -1;
    cap->env_offset = -1;
    cap->is_reassigned = false;
    cap->needs_cell = false;
    child_l.func->ncaptures = 1;

    XiValue *captured_plan =
        xi_value_new(child_l.func, child_l.cur_block, XI_LOAD_UPVAL, cap->type, 0);
    if (!captured_plan) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    captured_plan->aux_int = 0;
    captured_plan->line = (uint32_t) (node ? node->line : 0);

    if (!lower_parallel_plan_lifecycle_call(&child_l, node, captured_plan, "_end")) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    if (child_l.cur_block)
        xi_block_set_return(child_l.cur_block, NULL);

    XiFunc *result = NULL;
    if (!child_l.had_error && xi_lower_capture_source_vars(&child_l)) {
        result = child_l.func;
    } else {
        xi_func_free(child_l.func);
    }
    xi_lower_cleanup(&child_l);

    if (!result)
        return NULL;
    return parallel_call_child_closure(l, result, node ? node->line : 0);
}

static bool lower_parallel_plan_register_end_defer(XiLower *l, AstNode *node, XiValue *plan) {
    XiValue *end_closure = lower_parallel_plan_end_defer_closure(l, node, plan);
    if (!end_closure)
        return false;
    return xi_lower_defer_register_closure(l, end_closure, node ? node->line : 0);
}

static XiValue *parallel_plan_call_make_for_each(XiLower *l, AstNode *node, XiValue *plan,
                                                 AstNode *range, AstNode *body_node) {
    RangeNode *rn = &range->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    if (!start || !end || !plan)
        return NULL;

    if (!lower_parallel_plan_lifecycle_call(l, node, plan, "_begin")) {
        l->had_error = true;
        return NULL;
    }
    xi_lower_defer_scope_push(l);
    if (!lower_parallel_plan_register_end_defer(l, node, plan)) {
        xi_lower_defer_scope_pop_normal(l, node ? node->line : 0);
        l->had_error = true;
        return NULL;
    }

    struct XrType *plan_state_type = lower_parallel_plan_state_type(l, plan->type);
    struct XrType *states_type =
        xr_type_new_array(l->isolate, plan_state_type ? plan_state_type : l->type_any);
    XiValue *states = lower_emit_field_load(l, plan, "_states", states_type, node->line);
    XiValue *workers = lower_emit_len(l, states, node->line);
    if (!states || !workers)
        return NULL;

    FunctionDeclNode *body_expr = &parallel_call_unwrap_grouping(body_node)->as.function_expr;
    struct XrType *state_type = parallel_call_param_type(l, body_expr->params[0], plan_state_type);
    struct XrType *abi_types[3] = {state_type ? state_type : l->type_any, l->type_int, l->type_int};
    ParallelCallParamBinding bindings[2] = {{
                                                .param = body_expr->params[0],
                                                .abi_index = 0,
                                                .type = abi_types[0],
                                            },
                                            {
                                                .param = body_expr->params[1],
                                                .abi_index = 1,
                                                .type = l->type_int,
                                            }};
    XiFunc *body = parallel_call_lower_lambda_func(l, body_node, "plan_for_each_body", l->type_unit,
                                                   XI_NATIVE_CALLBACK_NONE, 3, abi_types, bindings,
                                                   2, node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = parallel_call_child_closure(l, body, node->line);
    if (!closure)
        return NULL;
    uint16_t ncap = body->ncaptures;

    XiParallelForData *data =
        (XiParallelForData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiParallelForData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = body;
    data->state_type = abi_types[0];
    data->state_name =
        body_expr->params[0] ? arena_strdup(l->func, body_expr->params[0]->name) : NULL;
    data->item_name =
        body_expr->params[1] ? arena_strdup(l->func, body_expr->params[1]->name) : NULL;
    data->state_symbol_id = body_expr->params[0] ? body_expr->params[0]->symbol_id : 0;
    data->item_symbol_id = body_expr->params[1] ? body_expr->params[1]->symbol_id : 0;
    data->body_child_index = (uint16_t) closure->aux_int;
    data->inclusive_end = rn->inclusive_end;
    data->range_body = false;
    data->plan_state = true;

    XiValue *par =
        xi_value_new(l->func, l->cur_block, XI_PAR_FOR, l->type_unit, (uint16_t) (5u + ncap));
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = closure;
    par->args[4] = states;
    for (uint16_t ci = 0; ci < ncap; ci++)
        par->args[5 + ci] = closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_FOR;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) node->line;

    xi_lower_insert_err_check(l, node, true);
    xi_lower_defer_scope_pop_normal(l, node ? node->line : 0);
    return par;
}

static XiValue *parallel_plan_call_make_map(XiLower *l, AstNode *node, XiValue *plan,
                                            AstNode *range, AstNode *body_node,
                                            XiValue *into_array) {
    RangeNode *rn = &range->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    if (!start || !end || !plan)
        return NULL;

    if (!lower_parallel_plan_lifecycle_call(l, node, plan, "_begin")) {
        l->had_error = true;
        return NULL;
    }
    xi_lower_defer_scope_push(l);
    if (!lower_parallel_plan_register_end_defer(l, node, plan)) {
        xi_lower_defer_scope_pop_normal(l, node ? node->line : 0);
        l->had_error = true;
        return NULL;
    }

    struct XrType *plan_state_type = lower_parallel_plan_state_type(l, plan->type);
    struct XrType *states_type =
        xr_type_new_array(l->isolate, plan_state_type ? plan_state_type : l->type_any);
    XiValue *states = lower_emit_field_load(l, plan, "_states", states_type, node->line);
    XiValue *workers = lower_emit_len(l, states, node->line);
    if (!states || !workers)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *elem_type = NULL;
    if (into_array && into_array->type && XR_TYPE_IS_ARRAY(into_array->type))
        elem_type = xi_get_container_elem_type(into_array->type);
    if (!elem_type && result_type && XR_TYPE_IS_ARRAY(result_type))
        elem_type = xi_get_container_elem_type(result_type);
    if (!elem_type)
        elem_type = l->type_any;
    if (!result_type || !XR_TYPE_IS_ARRAY(result_type))
        result_type = into_array && into_array->type ? into_array->type
                                                     : xr_type_new_array(l->isolate, elem_type);

    FunctionDeclNode *body_expr = &parallel_call_unwrap_grouping(body_node)->as.function_expr;
    struct XrType *state_type = parallel_call_param_type(l, body_expr->params[0], plan_state_type);
    struct XrType *abi_types[3] = {state_type ? state_type : l->type_any, l->type_int, l->type_int};
    ParallelCallParamBinding bindings[2] = {{
                                                .param = body_expr->params[0],
                                                .abi_index = 0,
                                                .type = abi_types[0],
                                            },
                                            {
                                                .param = body_expr->params[1],
                                                .abi_index = 1,
                                                .type = l->type_int,
                                            }};
    XiFunc *body = parallel_call_lower_lambda_func(l, body_node, "plan_map_body", elem_type,
                                                   XI_NATIVE_CALLBACK_NONE, 3, abi_types, bindings,
                                                   2, node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = parallel_call_child_closure(l, body, node->line);
    if (!closure)
        return NULL;
    uint16_t ncap = body->ncaptures;
    uint16_t extra_count = (uint16_t) (1u + (into_array ? 1u : 0u));
    uint16_t capture_base = (uint16_t) (4u + extra_count);

    XiParallelMapData *data =
        (XiParallelMapData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiParallelMapData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = body;
    data->element_type = elem_type;
    data->state_type = abi_types[0];
    data->state_name =
        body_expr->params[0] ? arena_strdup(l->func, body_expr->params[0]->name) : NULL;
    data->item_name =
        body_expr->params[1] ? arena_strdup(l->func, body_expr->params[1]->name) : NULL;
    data->state_symbol_id = body_expr->params[0] ? body_expr->params[0]->symbol_id : 0;
    data->item_symbol_id = body_expr->params[1] ? body_expr->params[1]->symbol_id : 0;
    data->body_child_index = (uint16_t) closure->aux_int;
    data->result_capture_index = ncap;
    data->start_capture_index = (uint16_t) (ncap + 1u);
    data->lane_count = 1;
    data->inclusive_end = rn->inclusive_end;
    data->into_result = into_array != NULL;
    data->plan_state = true;

    XiValue *par = xi_value_new(l->func, l->cur_block, XI_PAR_MAP, result_type,
                                (uint16_t) (capture_base + ncap));
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = closure;
    par->args[4] = states;
    if (into_array)
        par->args[5] = into_array;
    for (uint16_t ci = 0; ci < ncap; ci++)
        par->args[capture_base + ci] = closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_MAP;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                  XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) node->line;

    xi_lower_insert_err_check(l, node, true);
    xi_lower_defer_scope_pop_normal(l, node ? node->line : 0);

    if (!into_array)
        return par;
    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *parallel_plan_call_make_reduce(XiLower *l, AstNode *node, XiValue *plan,
                                               AstNode *range, AstNode *initial_node,
                                               AstNode *body_node, AstNode *combine_node) {
    RangeNode *rn = &range->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    XiValue *initial = xi_lower_expr(l, initial_node);
    if (!start || !end || !plan || !initial)
        return NULL;

    if (!lower_parallel_plan_lifecycle_call(l, node, plan, "_begin")) {
        l->had_error = true;
        return NULL;
    }
    xi_lower_defer_scope_push(l);
    if (!lower_parallel_plan_register_end_defer(l, node, plan)) {
        xi_lower_defer_scope_pop_normal(l, node ? node->line : 0);
        l->had_error = true;
        return NULL;
    }

    struct XrType *plan_state_type = lower_parallel_plan_state_type(l, plan->type);
    struct XrType *states_type =
        xr_type_new_array(l->isolate, plan_state_type ? plan_state_type : l->type_any);
    XiValue *states = lower_emit_field_load(l, plan, "_states", states_type, node->line);
    XiValue *workers = lower_emit_len(l, states, node->line);
    if (!states || !workers)
        return NULL;

    struct XrType *acc_type = xi_lower_node_type(l, node);
    if (!acc_type || XR_TYPE_IS_UNKNOWN(acc_type))
        acc_type = initial->type ? initial->type : l->type_int;
    initial = xi_lower_checktype_for_type(l, initial_node, initial, acc_type);
    bool native_i64 = acc_type && XR_TYPE_IS_INT(acc_type);
    bool native_agg = !native_i64 && xi_lower_type_struct_layout(l, acc_type) != NULL;
    XiNativeCallbackKind combine_callback = XI_NATIVE_CALLBACK_NONE;
    if (native_i64)
        combine_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE;
    else if (native_agg)
        combine_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE;

    FunctionDeclNode *body_expr = &parallel_call_unwrap_grouping(body_node)->as.function_expr;
    struct XrType *state_type = parallel_call_param_type(l, body_expr->params[0], plan_state_type);
    struct XrType *body_abi_types[3] = {state_type ? state_type : l->type_any, l->type_int,
                                        l->type_int};
    ParallelCallParamBinding body_bindings[2] = {{
                                                     .param = body_expr->params[0],
                                                     .abi_index = 0,
                                                     .type = body_abi_types[0],
                                                 },
                                                 {
                                                     .param = body_expr->params[1],
                                                     .abi_index = 1,
                                                     .type = l->type_int,
                                                 }};
    XiFunc *body = parallel_call_lower_lambda_func(l, body_node, "plan_reduce_body", acc_type,
                                                   XI_NATIVE_CALLBACK_NONE, 3, body_abi_types,
                                                   body_bindings, 2, node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }

    FunctionDeclNode *combine_expr = &parallel_call_unwrap_grouping(combine_node)->as.function_expr;
    struct XrType *combine_abi_types[2] = {acc_type, acc_type};
    ParallelCallParamBinding combine_bindings[2] = {{
                                                        .param = combine_expr->params[0],
                                                        .abi_index = 0,
                                                        .type = acc_type,
                                                    },
                                                    {
                                                        .param = combine_expr->params[1],
                                                        .abi_index = 1,
                                                        .type = acc_type,
                                                    }};
    XiFunc *combine = parallel_call_lower_lambda_func(
        l, combine_node, "plan_reduce_combine", acc_type, combine_callback, 2, combine_abi_types,
        combine_bindings, 2, node->line);
    if (!combine) {
        xi_func_free(body);
        l->had_error = true;
        return NULL;
    }

    XiValue *body_closure = parallel_call_child_closure(l, body, node->line);
    if (!body_closure) {
        xi_func_free(combine);
        return NULL;
    }
    XiValue *combine_closure = parallel_call_child_closure(l, combine, node->line);
    if (!combine_closure)
        return NULL;
    uint16_t body_ncap = body->ncaptures;
    uint16_t combine_ncap = combine->ncaptures;

    XiParallelReduceData *data = (XiParallelReduceData *) xi_func_arena_alloc(
        l->func, (uint32_t) sizeof(XiParallelReduceData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = body;
    data->combine_func = combine;
    data->accumulator_type = acc_type;
    data->state_type = body_abi_types[0];
    data->state_name =
        body_expr->params[0] ? arena_strdup(l->func, body_expr->params[0]->name) : NULL;
    data->item_name =
        body_expr->params[1] ? arena_strdup(l->func, body_expr->params[1]->name) : NULL;
    data->state_symbol_id = body_expr->params[0] ? body_expr->params[0]->symbol_id : 0;
    data->item_symbol_id = body_expr->params[1] ? body_expr->params[1]->symbol_id : 0;
    data->body_child_index = (uint16_t) body_closure->aux_int;
    data->combine_child_index = (uint16_t) combine_closure->aux_int;
    data->inclusive_end = rn->inclusive_end;
    data->range_body = false;
    data->plan_state = true;

    uint16_t nargs = (uint16_t) (7u + body_ncap + combine_ncap);
    XiValue *par = xi_value_new(l->func, l->cur_block, XI_PAR_REDUCE, acc_type, nargs);
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = initial;
    par->args[4] = body_closure;
    par->args[5] = combine_closure;
    par->args[6] = states;
    for (uint16_t ci = 0; ci < body_ncap; ci++)
        par->args[7 + ci] = body_closure->args[ci];
    for (uint16_t ci = 0; ci < combine_ncap; ci++)
        par->args[7 + body_ncap + ci] = combine_closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_REDUCE;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                  XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) node->line;

    xi_lower_insert_err_check(l, node, true);
    xi_lower_defer_scope_pop_normal(l, node ? node->line : 0);
    return par;
}

static void parallel_plan_intrinsic_error(XiLower *l, AstNode *node, const char *message) {
    if (l)
        l->had_error = true;
    fprintf(stderr, "error: %s at line %d\n", message, node ? node->line : -1);
}

static XiValue *lower_parallel_plan_intrinsic_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                   XiValue *plan, XaParallelCallKind kind,
                                                   XaIntrinsicId intrinsic_id) {
    if (!l || !node || !call || !plan || kind == XA_PAR_CALL_NONE)
        return NULL;
    uint32_t first_value_id = l->func ? l->func->next_value_id : 0;

    if (kind == XA_PAR_CALL_FOR_EACH) {
        if (call->arg_count != 2) {
            parallel_plan_intrinsic_error(
                l, node, "parallel.Plan.forEach expected (Range, inline (state, item) lambda)");
            return NULL;
        }
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[1], 2)) {
            parallel_plan_intrinsic_error(
                l, node, "parallel.Plan.forEach expected a Range and inline (state, item) lambda");
            return NULL;
        }
        XiValue *result =
            parallel_plan_call_make_for_each(l, node, plan, range, call->arguments[1]);
        return lower_bind_parallel_intrinsic_result(l, first_value_id, intrinsic_id, result);
    }

    if (kind == XA_PAR_CALL_MAP) {
        if (call->arg_count != 2) {
            parallel_plan_intrinsic_error(
                l, node, "parallel.Plan.map expected (Range, inline (state, item) lambda)");
            return NULL;
        }
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[1], 2)) {
            parallel_plan_intrinsic_error(
                l, node, "parallel.Plan.map expected a Range and inline (state, item) lambda");
            return NULL;
        }
        XiValue *result =
            parallel_plan_call_make_map(l, node, plan, range, call->arguments[1], NULL);
        return lower_bind_parallel_intrinsic_result(l, first_value_id, intrinsic_id, result);
    }

    if (kind == XA_PAR_CALL_MAP_INTO) {
        if (call->arg_count != 3) {
            parallel_plan_intrinsic_error(
                l, node,
                "parallel.Plan.mapInto expected (Range, output, inline (state, item) lambda)");
            return NULL;
        }
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[2], 2)) {
            parallel_plan_intrinsic_error(
                l, node,
                "parallel.Plan.mapInto expected a Range, output array, and inline (state, item) "
                "lambda");
            return NULL;
        }
        XiValue *into = xi_lower_expr(l, call->arguments[1]);
        if (!into)
            return NULL;
        XiValue *result =
            parallel_plan_call_make_map(l, node, plan, range, call->arguments[2], into);
        return lower_bind_parallel_intrinsic_result(l, first_value_id, intrinsic_id, result);
    }

    if (kind == XA_PAR_CALL_REDUCE) {
        if (call->arg_count != 4) {
            parallel_plan_intrinsic_error(l, node,
                                          "parallel.Plan.reduce expected (Range, initial, inline "
                                          "(state, item) body, inline combine lambda)");
            return NULL;
        }
        AstNode *range = parallel_call_unwrap_grouping(call->arguments[0]);
        if (!range || range->type != AST_RANGE ||
            !parallel_call_is_plain_lambda(call->arguments[2], 2) ||
            !parallel_call_is_plain_lambda(call->arguments[3], 2)) {
            parallel_plan_intrinsic_error(l, node,
                                          "parallel.Plan.reduce expected a Range, inline (state, "
                                          "item) body, and inline combine lambda");
            return NULL;
        }
        XiValue *result = parallel_plan_call_make_reduce(l, node, plan, range, call->arguments[1],
                                                         call->arguments[2], call->arguments[3]);
        return lower_bind_parallel_intrinsic_result(l, first_value_id, intrinsic_id, result);
    }

    return NULL;
}

/* Shared construction lowering used by both `T(args)` calls (lower_call) and
 * the legacy new-expr node. Builds built-in collection ops or a class
 * constructor invocation. result_type is the resolved instance/container type
 * from the node table. */
static XiValue *lower_construct(XiLower *l, AstNode *node, struct XrType *result_type,
                                const char *module_name, const char *cname, AstNode **arguments,
                                XrCallArgAccess *arg_accesses, int arg_count) {
    XR_DCHECK(cname != NULL, "construct must have class name");

    /* The parser rewrites `Map(..)`, `Array(..)`, `StringBuilder(..)` and the
     * rest of xr_is_construct_only_type_name() into a new-expr before any
     * scope exists, so the name alone cannot say which type is meant. The
     * analyzer has since resolved it: a user class that shadows the builtin
     * yields an instance type carrying its XrClassInfo, and constructing the
     * builtin here would leave the runtime object disagreeing with that
     * static type. Fall straight through to the ordinary constructor call. */
    bool user_class_shadows_builtin = result_type && result_type->kind == XR_KIND_INSTANCE &&
                                      result_type->instance.class_ref != NULL;

    /* Built-in collection types: emit specialized ops (no constructor call) */
    if (module_name == NULL && !user_class_shadows_builtin) {
        if (strcmp(cname, "Map") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            /* Encode key_kind/value_tid/storage: C = (key_kind<<8)|(vtid<<3)|flags */
            if (XR_TYPE_IS_MAP(result_type)) {
                uint8_t vtid = 0, key_kind = 0;
                if (result_type->map.value_type)
                    vtid = xr_type_to_tid(result_type->map.value_type);
                if (result_type->map.key_type) {
                    uint8_t ktid = xr_type_to_tid(result_type->map.key_type);
                    if (ktid == XR_TID_STRING)
                        key_kind = 1;
                    else if (ktid == XR_TID_INT)
                        key_kind = 2;
                }
                v->aux_int = (int64_t) ((key_kind << 8) | ((vtid & 0x1F) << 3));
            } else {
                v->aux_int = 0;
            }
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            /* Encode elem_tid from explicit type param: C = (tid<<2)|mode */
            if (XR_TYPE_IS_ARRAY(result_type) && result_type->container.element_type) {
                uint8_t tid = xr_type_to_tid(result_type->container.element_type);
                v->aux_int = (int64_t) (tid << 2);
            }
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && arg_count == 1) {
            XiValue *arg = xi_lower_expr(l, arguments[0]);
            if (!arg)
                return NULL;
            bool array_copy = result_type && XR_TYPE_IS_ARRAY(result_type) && arg->type &&
                              !XR_TYPE_IS_INT(arg->type);
            if (!array_copy && arg->type && !XR_TYPE_IS_INT(arg->type))
                goto generic_constructor;
            XiValue *v = xi_value_new(l->func, l->cur_block,
                                      array_copy ? XI_CALL_BUILTIN : XI_ARRAY_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = arg;
            if (array_copy)
                v->aux = (void *) "array_copy_new";
            v->aux_int = xi_array_cfield_from_type(result_type);
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && arg_count == 2) {
            XiValue *count = xi_lower_expr(l, arguments[0]);
            XiValue *fill = xi_lower_expr(l, arguments[1]);
            if (!count || !fill)
                return NULL;
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = count;
            v->args[1] = fill;
            v->aux = (void *) "array_filled_new";
            v->aux_int = xi_array_cfield_from_type(result_type);
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Set") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            /* Encode elem_tid from explicit type param: B = (tid<<3)|flags */
            if (result_type->kind == XR_KIND_SET && result_type->container.element_type) {
                uint8_t tid = xr_type_to_tid(result_type->container.element_type);
                v->aux_int = (int64_t) ((tid & 0x1F) << 3);
            } else {
                v->aux_int = 0;
            }
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "StringBuilder") == 0 && arg_count == 0) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 0);
            if (!v)
                return NULL;
            v->aux = (void *) "StringBuilder";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        /* Exception: no special handling needed — it is a regular class with a
         * primitive constructor registered in core->panicInfoClass. Falls through
         * to the generic class-instantiation path below. */
        /* new Channel() / new Channel(bufferSize) */
        if (strcmp(cname, "Channel") == 0 && arg_count <= 1) {
            XiValue *buf_size = arg_count == 1 ? xi_lower_expr(l, arguments[0]) : NULL;
            uint8_t elem_tid = 0;
            if (result_type && result_type->kind == XR_KIND_CHANNEL &&
                result_type->container.element_type) {
                elem_tid = xr_type_to_tid(result_type->container.element_type);
                if (!buf_size)
                    buf_size = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            }
            uint16_t nch = buf_size ? 1 : 0;
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_NEW, result_type, nch);
            if (!v)
                return NULL;
            if (buf_size)
                v->args[0] = buf_size;
            v->aux_int = elem_tid;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

generic_constructor:;
    /* Generic class: resolve class name and invoke constructor */
    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    for (int i = 0; i < arg_count; i++) {
        XiValue *arg = xi_lower_expr(l, arguments[i]);
        if (!arg)
            return NULL;
        if (!xi_lower_arg_list_push(l, &args, arg, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
    }
    XiValue **arg_vals = args.items;
    int n = args.count;

    XiValue *cls = NULL;
    XaSymbol *class_sym = xi_lower_lookup_class_symbol(l, cname);
    XaSymbolLinks *class_links =
        (class_sym && l->analyzer) ? xa_analyzer_get_links(l->analyzer, class_sym) : NULL;
    struct XrType *constructor_type = xi_lower_class_constructor_type(l, class_sym);
    if (!constructor_type)
        constructor_type = xi_lower_type_constructor_type(l, result_type);
    XrParamMode stack_constructor_modes[64];
    const XrParamMode *constructor_modes = NULL;
    int constructor_pcount = 0;
    if (constructor_type) {
        constructor_modes = lower_function_param_modes(
            l, constructor_type, stack_constructor_modes,
            (int) (sizeof(stack_constructor_modes) / sizeof(stack_constructor_modes[0])),
            &constructor_pcount);
    }
    bool has_user_class_info = class_links && class_links->class_info != NULL;
    bool force_builtin_class =
        ((module_name && strcmp(module_name, "sync") == 0 &&
          xi_lower_sync_runtime_class_name(cname)) ||
         (class_sym && xi_lower_symbol_is_sync_runtime_class(l, class_sym->id, cname)) ||
         (xi_lower_sync_runtime_class_name(cname) && !has_user_class_info));
    if (!force_builtin_class) {
        int var_id = xi_lower_var_find(l, 0, cname);
        if (var_id >= 0) {
            if (l->is_program && l->shared_map[var_id] >= 0) {
                XiTopBinding b;
                b.slot = l->shared_map[var_id];
                b.name = l->vars[var_id].name;
                b.type = l->vars[var_id].type;
                cls = xi_lower_emit_top_load(l, b, l->type_any);
            } else {
                cls = xi_lower_braun_read(l, var_id, l->cur_block);
            }
        }
    }
    if (!cls && !force_builtin_class) {
        XiTopBinding tb = xi_lower_find_top_binding(l, 0, cname);
        if (xi_top_binding_valid(tb))
            cls = xi_lower_emit_top_load(l, tb, l->type_any);
    }
    if (!cls && !force_builtin_class) {
        struct XrType *upval_type = NULL;
        int upval_idx = xi_lower_resolve_upvalue(l, 0, cname, &upval_type);
        if (upval_idx >= 0) {
            cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, l->type_any, 0);
            if (cls)
                cls->aux_int = upval_idx;
        }
    }
    /* Named-imported generic class construction is rewritten by
     * monomorphization to the concrete export name (`Foo$i64(args)`), while
     * the source import binding remains under the generic origin name
     * (`import { Foo } ...`).  If no local/top/upvalue class object exists,
     * preserve the cross-module provenance by emitting an import ref for the
     * monomorphic export itself instead of falling through to a null receiver.
     * AOT then resolves the constructor through the normal import/export table
     * using the same metadata path as namespace imports. */
    if (!cls && !force_builtin_class && module_name == NULL && class_sym && cname &&
        strchr(cname, '$')) {
        const char *module_path = xi_lower_export_module_for_symbol(l, class_sym, cname);
        if (module_path)
            cls = xi_lower_emit_import_ref(l, module_path, cname,
                                           class_links ? class_links->type : l->type_any,
                                           node ? (int) node->line : 0);
    }
    /* Built-in unified-class names (Exception, Range, sync classes, etc.)
     * are populated into the VM builtins array by the prelude module
     * loader at fixed XR_GLOBAL_VAR_* indices. Resolve them via
     * XI_GET_BUILTIN before falling back to null. */
    if (!cls && cname && !user_class_shadows_builtin)
        cls = xi_lower_emit_builtin_class(l, cname, node->line);
    if (!cls) {
        cls = xi_const_null(l->func, l->cur_block, l->type_null);
    }

    /* Zero-arg struct with compile-time layout → XI_AGG_NEW.
     * The emitter decides stack vs heap via struct_can_stack_alloc. */
    if (arg_count == 0 && module_name == NULL && l->analyzer) {
        XrAggregateLayout *slayout = xi_lower_lookup_struct_layout(l, cname);
        if (slayout) {
            XiValue *inst = xi_value_new(l->func, l->cur_block, XI_AGG_NEW, result_type, 1);
            if (!inst)
                return NULL;
            inst->args[0] = cls;
            inst->aux = (void *) slayout;
            inst->flags |= XI_FLAG_SIDE_EFFECT;
            inst->line = (uint32_t) node->line;
            return inst;
        }
    }

    CallExprNode call_view = {
        .arguments = arguments,
        .arg_accesses = arg_accesses,
        .arg_count = arg_count,
    };
    XiCallWriteback *writebacks = NULL;
    XiCallPlan *call_plan =
        lower_build_call_plan(l, &call_view, arg_vals, n, constructor_modes, constructor_pcount,
                              NULL, 0, constructor_type, &writebacks, (int) node->line);
    if (l->had_error)
        return NULL;

    uint16_t nargs = (uint16_t) (n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
    if (!call)
        return NULL;
    call->args[0] = cls;
    for (int i = 0; i < n; i++)
        call->args[i + 1] = arg_vals[i];
    call->aux = (void *) "constructor";
    call->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
    /* Same fresh-result fact as the class-binding call path: a user class
     * allocates its instance here.  A builtin class reaches this path too and
     * stays unmarked, since its `call` static method may return an existing
     * object. */
    if (has_user_class_info && result_type && result_type->kind == XR_KIND_INSTANCE)
        call->lowering_flags |= XI_LOWERING_FLAG_CONSTRUCTOR_CALL;
    call->call_plan = call_plan;
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    call->line = (uint32_t) node->line;
    xi_lower_bind_callsite_id(l, call, xi_lower_source_node_id(l, node));
    xi_lower_insert_err_check(l, node, true);
    if (!lower_apply_call_writebacks(l, call_plan, writebacks, (int) node->line))
        return NULL;
    return call;
}

static XiValue *lower_new_expr(XiLower *l, AstNode *node) {
    NewExprNode *ne = &node->as.new_expr;
    return lower_construct(l, node, xi_lower_node_type(l, node), ne->module_name, ne->class_name,
                           ne->arguments, ne->arg_accesses, ne->arg_count);
}

/* True if the named class is `Exception` or derives from it. Exception is a
 * built-in primitive class; constructing it (or a subclass) must go through the
 * new-expr construction path, not the normal class-binding call path. */
static bool lower_class_is_exception_kind(XiLower *l, const char *name) {
    if (!name)
        return false;
    if (strcmp(name, "PanicInfo") == 0)
        return true;
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym || !l->analyzer)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    XrClassInfo *info = links ? links->class_info : NULL;
    for (XrClassInfo *c = info; c; c = c->base) {
        if (c->base_name && strcmp(c->base_name, "PanicInfo") == 0)
            return true;
        if (c->name && strcmp(c->name, "PanicInfo") == 0)
            return true;
    }
    return false;
}

/* True if the named class declares type parameters (generic). Generic classes
 * must construct through the new-expr path (monomorphization-aware), which the
 * normal class-binding call path does not handle for AOT. */
static bool lower_class_is_generic(XiLower *l, const char *name) {
    if (!name || !l->analyzer)
        return false;
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return links && xa_symbol_links_get_type_param_count(links) > 0;
}

/* A bare `T(args)` call constructs through the new-expr construction path when
 * the normal class-binding call path cannot handle it: Exception (built-in
 * primitive class) and its subclasses, and generic classes (monomorphization).
 * Plain non-generic user classes (top-level and nested) construct correctly via
 * the normal call lowering and are left alone (preserves cross-module dispatch).
 * Returns NULL when the normal call path should be used. */
static XiValue *lower_try_construct_call(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!call->callee || call->callee->type != AST_VARIABLE)
        return NULL;
    const char *name = call->callee->as.variable.name;
    if (!name)
        return NULL;
    if (xi_lower_symbol_is_sync_runtime_class(l, call->callee->as.variable.symbol_id, name))
        return NULL;
    bool generic_class_call = lower_class_is_generic(l, name);
    if (!generic_class_call && call->type_arg_count > 0 && xi_lower_lookup_class_symbol(l, name))
        generic_class_call = true;
    if (!generic_class_call && strchr(name, '$') && xi_lower_lookup_class_symbol(l, name))
        generic_class_call = true;
    if (!lower_class_is_exception_kind(l, name) && !generic_class_call)
        return NULL;
    return lower_construct(l, node, xi_lower_node_type(l, node), NULL, name, call->arguments,
                           call->arg_accesses, call->arg_count);
}

static XiValue *lower_go_expr(XiLower *l, AstNode *node) {
    GoExprNode *go = &node->as.go_expr;
    AstNode *expr = go->expr;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiOp spawn_op = go->spawn_kind == XR_SPAWN_THREAD ? XI_THREAD_SPAWN : XI_GO;

    if (expr->type == AST_CALL_EXPR) {
        /* go fn(args): extract callee + args, don't execute the call.
         * XI_GO args[0]=callee, args[1..n]=params → emits OP_GO.
         * Lower ALL operands before creating XI_GO so they precede it
         * in the block's values array (same pattern as lower_call). */
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee)
            return NULL;
        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        uint8_t stack_modes[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerGoArgList args;
        xi_lower_go_arg_list_init(&args, stack_args, stack_modes, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_go_call_args(l, call, &args, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;
        uint16_t nargs = (uint16_t) (1 + n);
        XiValue *v = xi_value_new(l->func, l->cur_block, spawn_op, result_type, nargs);
        if (!v)
            return NULL;
        v->args[0] = callee;
        for (int i = 0; i < n; i++) {
            v->args[1 + i] = arg_vals[i];
        }
        if (n > 0) {
            uint8_t *modes =
                (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) n * sizeof(uint8_t)));
            if (!modes)
                return NULL;
            memcpy(modes, args.modes, (size_t) n * sizeof(uint8_t));
            v->aux = modes;
        }
        v->aux_int = (int64_t) pack_go_aux((int) go->link_mode);
        if (spawn_op == XI_GO && xa_task_type_requires_shared_copy_publication(result_type))
            v->aux_int |= XI_GO_AUX_RESULT_COPY_SHARED;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    /* go fn — closure with no arguments */
    XiValue *callee = xi_lower_expr(l, expr);
    if (!callee)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, spawn_op, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = callee;
    v->aux_int = (int64_t) pack_go_aux((int) go->link_mode);
    if (spawn_op == XI_GO && xa_task_type_requires_shared_copy_publication(result_type))
        v->aux_int |= XI_GO_AUX_RESULT_COPY_SHARED;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_await_expr(XiLower *l, AstNode *node) {
    AwaitExprNode *aw = &node->as.await_expr;
    bool direct_temporary_task = aw->expr && aw->expr->type == AST_GO_EXPR &&
                                 aw->expr->as.go_expr.link_mode == 0 && !aw->timeout &&
                                 !aw->is_any && !aw->is_all && !aw->is_any_success && !aw->into;
    struct XrType *result_type = xi_lower_node_type(l, node);
    bool consumes_unique_task = !aw->is_any && !aw->is_all && !aw->is_any_success && !aw->into &&
                                xa_task_result_requires_consuming_await(result_type);
    XiValue *task = xi_lower_expr(l, aw->expr);
    if (!task)
        return NULL;
    XiValue *into = aw->into ? xi_lower_expr(l, aw->into) : NULL;
    if (aw->into && !into)
        return NULL;

    /* Optional timeout argument */
    XiValue *timeout = aw->timeout ? xi_lower_expr(l, aw->timeout) : NULL;
    uint16_t nargs = (uint16_t) (1 + (into ? 1 : 0) + (timeout ? 1 : 0));

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AWAIT, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = task;
    uint16_t argi = 1;
    if (into)
        v->args[argi++] = into;
    if (timeout)
        v->args[argi++] = timeout;
    /* Encode await variant flags. */
    v->aux_int = (aw->is_any ? XI_AWAIT_AUX_ANY : 0) | (aw->is_all ? XI_AWAIT_AUX_ALL : 0) |
                 (aw->is_any_success ? XI_AWAIT_AUX_ANY_SUCCESS : 0) |
                 ((direct_temporary_task || consumes_unique_task) ? XI_AWAIT_AUX_CONSUME_TASK : 0) |
                 (into ? XI_AWAIT_AUX_INTO_RESULT : 0);
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    if (into)
        v->flags |= XI_FLAG_WRITES_MEM;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_channel_new(XiLower *l, AstNode *node) {
    ChannelNewNode *ch = &node->as.channel_new;
    XiValue *buf_size = ch->buffer_size ? xi_lower_expr(l, ch->buffer_size) : NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    uint8_t elem_tid = 0;
    if (result_type && result_type->kind == XR_KIND_CHANNEL &&
        result_type->container.element_type) {
        elem_tid = xr_type_to_tid(result_type->container.element_type);
        if (!buf_size)
            buf_size = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    }
    uint16_t nargs = buf_size ? 1 : 0;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_NEW, result_type, nargs);
    if (!v)
        return NULL;
    if (buf_size)
        v->args[0] = buf_size;
    v->aux_int = elem_tid;
    v->line = (uint32_t) node->line;
    return v;
}

/*
 * Template string: "hello ${name}, age ${age}"
 * parts = ["hello ", <name_expr>, ", age ", <age_expr>]
 * Lower each part, then STR_CONCAT all.
 */
static XiValue *lower_template_string(XiLower *l, AstNode *node) {
    TemplateStringNode *ts = &node->as.template_str;
    int count = ts->part_count;
    if (count < 0 || count > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] template string part count exceeds %d at line %d\n",
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }

    /* Evaluate all parts */
    XiValue *stack_parts[XI_LOWER_VALUE_LIST_STACK_CAP];
    XiLowerValueList parts;
    xi_lower_value_list_init(&parts, stack_parts, XI_LOWER_VALUE_LIST_STACK_CAP);
    int n = count;
    for (int i = 0; i < n; i++) {
        XiValue *part = xi_lower_expr(l, ts->parts[i]);
        if (!part)
            return NULL;
        if (!xi_lower_value_list_push(l, &parts, part, XI_LOWER_MAX_VARIADIC_VALUES,
                                      "template string part count", node->line))
            return NULL;
    }

    struct XrType *result_type = l->type_string;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STR_CONCAT, result_type, (uint16_t) n);
    if (!v)
        return NULL;
    for (int i = 0; i < n; i++) {
        v->args[i] = parts.items[i];
    }
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_set_literal(XiLower *l, AstNode *node) {
    SetLiteralNode *sl = &node->as.set_literal;
    int count = sl->count;

    int n = count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, sl->elements[i]);
        if (!elem_vals[i])
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (result_type && XR_TYPE_IS_SET(result_type)) {
        for (int i = 0; i < n; i++) {
            elem_vals[i] = xi_lower_apply_numeric_conversion_witness(
                l, sl->elements[i], elem_vals[i], result_type->container.element_type);
            if (!elem_vals[i])
                return NULL;
            elem_vals[i] = xi_lower_narrow_for_static_type(l, sl->elements[i], elem_vals[i],
                                                           result_type->container.element_type);
        }
    }
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *set_val = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
    if (!set_val)
        return NULL;
    set_val->args[0] = cap;
    set_val->line = (uint32_t) node->line;
    xi_lower_bind_map_shape_id(l, set_val, (uint32_t) node->line, XG_MAP_CONTAINER_SET);
    if (set_val->xg_map_shape_id != XG_NO_ID) {
        XiMapLiteralData *data =
            (XiMapLiteralData *) xi_func_arena_alloc(l->func, (uint32_t) sizeof(XiMapLiteralData));
        if (!data)
            return NULL;
        data->keys = elem_vals;
        data->values = NULL;
        data->count = (uint16_t) count;
        data->container_kind = XG_MAP_CONTAINER_SET;
        set_val->aux = data;
        set_val->aux_kind = XI_AUX_KIND_MAP_LITERAL;
    }

    /* Populate: CALL_METHOD("add") for each element */
    for (int i = 0; i < n; i++) {
        XiValue *add = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_unit, 2);
        if (!add)
            break;
        add->args[0] = set_val;
        add->args[1] = elem_vals[i];
        add->aux = (void *) "add";
        add->aux_int = (int64_t) xi_lower_method_symbol(l, "add") << 1;
        add->flags |= XI_FLAG_SIDE_EFFECT;
        add->xg_map_shape_id = set_val->xg_map_shape_id;
    }
    return set_val;
}

static XiValue *lower_typed_enum_metadata_is_test(XiLower *l, XiValue *value, XrType *target_type) {
    if (!xr_type_is_enum_metadata(target_type))
        return NULL;
    XrType *source_owner = value->enum_metadata_owner ? value->enum_metadata_owner
                                                      : xr_type_enum_metadata_owner(value->type);
    XrEnumMetadataKind source_kind = value->enum_metadata_kind != XR_ENUM_METADATA_NONE
                                         ? (XrEnumMetadataKind) value->enum_metadata_kind
                                         : xr_type_enum_metadata_kind(value->type);
    XrType *target_owner = xr_type_enum_metadata_owner(target_type);
    XrEnumMetadataKind target_kind = xr_type_enum_metadata_kind(target_type);
    if (!source_owner || source_kind == XR_ENUM_METADATA_NONE || !target_owner ||
        target_kind == XR_ENUM_METADATA_NONE)
        return NULL;
    uint32_t source_layout = source_owner->kind == XR_KIND_ENUM && source_owner->enum_type.layout
                                 ? source_owner->enum_type.layout->layout_id
                                 : 0;
    uint32_t target_layout = target_owner->kind == XR_KIND_ENUM && target_owner->enum_type.layout
                                 ? target_owner->enum_type.layout->layout_id
                                 : 0;
    if (source_layout == 0 || target_layout == 0)
        return NULL;
    return xi_const_bool(l->func, l->cur_block,
                         source_layout == target_layout && source_kind == target_kind,
                         l->type_bool);
}

/* Emit an XI_IS test against the given XrTypeRef for an existing value.
 * Used both by `expr is T` and by `is T` patterns in match arms. */
static XiValue *lower_null_guard_or_throw(XiLower *l, XiValue *val, struct XrType *result_type,
                                          const char *message, int line);
static XiValue *lower_record_shape_narrow(XiLower *l, XiValue *val, struct XrType *record_type,
                                          int line);
static bool xi_type_is_checkable_record(struct XrType *type);
static bool xi_type_may_carry_record_shape(struct XrType *type);

/* A Record test compares field sets, not a type id: every object-shaped value
 * carries the same runtime type id, so the shared structural check is the only
 * thing that can answer it. Reuse the validated narrowing the cast path uses
 * and keep just its success bit. Returns NULL when the target is not a Record
 * whose field set is known, leaving the type-id path to handle it. */
static XiValue *lower_record_shape_is_test(XiLower *l, XiValue *val, struct XrType *target_type,
                                           int line) {
    if (!val || !xi_type_is_checkable_record(target_type) ||
        !xi_type_may_carry_record_shape(val->type))
        return NULL;
    XiValue *narrowed = lower_record_shape_narrow(l, val, target_type, line);
    if (!narrowed)
        return NULL;
    XiValue *isnull = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!isnull)
        return NULL;
    isnull->args[0] = narrowed;
    isnull->line = (uint32_t) line;
    XiValue *matched = xi_value_new(l->func, l->cur_block, XI_NOT, l->type_bool, 1);
    if (!matched)
        return NULL;
    matched->args[0] = isnull;
    matched->line = (uint32_t) line;
    return matched;
}

XR_FUNC XiValue *xi_lower_is_test(XiLower *l, XiValue *val, XrTypeRef *tref, int line) {
    if (!val)
        return NULL;

    XrType *target_type =
        (tref && l && l->analyzer) ? xr_tref_resolve_in_analyzer(l->analyzer, tref) : NULL;

    /* Typed descriptors have a statically known owner and kind, so this test
     * folds without interpreting the scalar as an erased descriptor box. */
    XiValue *enum_metadata_test = lower_typed_enum_metadata_is_test(l, val, target_type);
    if (enum_metadata_test)
        return enum_metadata_test;

    XiValue *record_shape_test = lower_record_shape_is_test(l, val, target_type, line);
    if (record_shape_test)
        return record_shape_test;

    /* Resolve the target type to a runtime value so the VM can use it
     * directly from a register:
     *   - Primitive types → XI_CONST with XrTypeId
     *   - Named types (classes) → scope-resolved class value */
    XiValue *type_val = NULL;
    if (tref) {
        int tid = -1;
        switch (tref->kind) {
            case XR_TREF_INT:
                tid = 8;
                break; /* XR_TID_INT */
            case XR_TREF_FLOAT:
                tid = 11;
                break; /* XR_TID_FLOAT */
            case XR_TREF_STRING:
                tid = 12;
                break; /* XR_TID_STRING */
            case XR_TREF_BOOL:
                tid = 1;
                break; /* XR_TID_BOOL */
            case XR_TREF_NULL:
                tid = 0;
                break; /* XR_TID_NULL */
            case XR_TREF_ERROR:
            case XR_TREF_INT_WIDTH:
            case XR_TREF_FLOAT_WIDTH:
            case XR_TREF_NAMED:
            case XR_TREF_GENERIC:
            case XR_TREF_OPTIONAL:
            case XR_TREF_UNION:
            case XR_TREF_FUNCTION:
            case XR_TREF_TUPLE:
            case XR_TREF_OBJECT:
            case XR_TREF_FIXED_ARRAY:
            case XR_TREF_TYPE_PARAM:
                break;
        }
        /* Generic containers: Array<T> → XR_TID_ARRAY, Map<K,V> → XR_TID_MAP, etc. */
        if (tid < 0 && tref->kind == XR_TREF_GENERIC && tref->name) {
            if (strcmp(tref->name, "Array") == 0)
                tid = 14; /* XR_TID_ARRAY */
            else if (strcmp(tref->name, "Map") == 0)
                tid = 16; /* XR_TID_MAP */
            else if (strcmp(tref->name, "Set") == 0)
                tid = 15; /* XR_TID_SET */
        }
        /* Bare container names without generic args and prelude types */
        if (tid < 0 && tref->kind == XR_TREF_NAMED && tref->name) {
            if (strcmp(tref->name, "Array") == 0)
                tid = 14;
            else if (strcmp(tref->name, "Map") == 0)
                tid = 16;
            else if (strcmp(tref->name, "Set") == 0)
                tid = 15;
            else if (strcmp(tref->name, "Json") == 0)
                tid = 18;
            else if (strcmp(tref->name, "PanicInfo") == 0)
                tid = 24; /* XR_TID_PANIC_INFO */
        }
        /* Tuple type: (T1, T2, ...) → look up TupleN class by arity */
        if (tid < 0 && tref->kind == XR_TREF_TUPLE && l->isolate) {
            uint16_t arity = (uint16_t) tref->nchildren;
            XrClass *tuple_cls = xr_get_or_create_tuple_class(l->isolate, arity);
            if (tuple_cls) {
                type_val = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_any, 0);
                if (type_val)
                    type_val->aux = (void *) tuple_cls;
            }
        }
        if (tid >= 0) {
            type_val = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_int, 0);
            if (type_val)
                type_val->aux_int = tid;
        } else if (xr_type_is_enum_metadata(target_type)) {
            /* Enum descriptor type tests use a backend-neutral token rather
             * than a runtime class namespace.  The token is consumed only by
             * XI_IS and never exposed as a source value. */
            type_val = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_int, 0);
            if (type_val)
                type_val->aux_int = xr_type_enum_metadata_token(target_type);
        } else if (tref->kind == XR_TREF_NAMED && tref->name) {
            XaSymbol *named_symbol = xa_analyzer_lookup(l->analyzer, tref->name);
            if (!named_symbol)
                named_symbol =
                    xa_analyzer_lookup_in_scope(l->analyzer, tref->name, l->analyzer->global_scope);
            if (!named_symbol)
                named_symbol = xa_analyzer_lookup_deep(l->analyzer, tref->name);
            if (named_symbol && named_symbol->kind == XA_SYM_ENUM)
                type_val = xi_lower_enum_namespace_value(l, named_symbol, tref->name, line);

            /* Resolve class from scope chain */
            int var = type_val ? -1 : xi_lower_var_find(l, 0, tref->name);
            if (!type_val && var >= 0) {
                if (l->is_program && var < l->var_count && l->shared_map[var] >= 0) {
                    XiTopBinding b;
                    b.slot = l->shared_map[var];
                    b.name = l->vars[var].name;
                    b.type = l->vars[var].type;
                    type_val = xi_lower_emit_top_load(l, b, l->type_any);
                } else {
                    type_val = xi_lower_braun_read(l, var, l->cur_block);
                }
            }
            if (!type_val) {
                XiTopBinding tb = xi_lower_find_top_binding(l, 0, tref->name);
                if (xi_top_binding_valid(tb))
                    type_val = xi_lower_emit_top_load(l, tb, l->type_any);
            }
            if (!type_val) {
                int builtin_index = xi_lower_builtin_class_global_index(tref->name);
                if (builtin_index >= 0) {
                    type_val = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN,
                                            target_type ? target_type : l->type_any, 0);
                    if (type_val) {
                        type_val->aux_int = builtin_index;
                        type_val->aux = (void *) arena_strdup(l->func, tref->name);
                        type_val->line = (uint32_t) line;
                    }
                }
            }
        }
    }

    uint16_t nargs = (type_val != NULL) ? 2 : 1;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_IS, l->type_bool, nargs);
    if (!v)
        return NULL;
    v->args[0] = val;
    if (type_val)
        v->args[1] = type_val;
    v->aux = (void *) target_type;
    v->line = (uint32_t) line;
    return v;
}

static XiValue *lower_is_expr(XiLower *l, AstNode *node) {
    IsExprNode *is = &node->as.is_expr;
    XiValue *val = xi_lower_expr(l, is->expr);
    if (!val)
        return NULL;
    return xi_lower_is_test(l, val, is->type, node->line);
}

static void lower_dynamic_as_target(XrTypeRef *tref, int *out_tid, const char **out_name) {
    int tid = -1;
    const char *name = "unknown";
    if (!tref) {
        *out_tid = tid;
        *out_name = name;
        return;
    }

    XrTypeRef *inner = tref;
    if (tref->kind == XR_TREF_OPTIONAL && tref->nchildren > 0)
        inner = tref->children[0];
    switch (inner->kind) {
        case XR_TREF_INT:
            tid = 8;
            name = "int";
            break;
        case XR_TREF_FLOAT:
            tid = 11;
            name = "float";
            break;
        case XR_TREF_STRING:
            tid = 12;
            name = "string";
            break;
        case XR_TREF_BOOL:
            tid = 1;
            name = "bool";
            break;
        case XR_TREF_RUNE:
            tid = XR_TID_RUNE;
            name = "rune";
            break;
        case XR_TREF_NULL:
            tid = 0;
            name = "null";
            break;
        case XR_TREF_ERROR:
        case XR_TREF_INT_WIDTH:
        case XR_TREF_FLOAT_WIDTH:
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
        case XR_TREF_OPTIONAL:
        case XR_TREF_UNION:
        case XR_TREF_FUNCTION:
        case XR_TREF_TUPLE:
        case XR_TREF_OBJECT:
        case XR_TREF_FIXED_ARRAY:
        case XR_TREF_TYPE_PARAM:
            break;
    }
    if (tid < 0 && inner->kind == XR_TREF_NAMED && inner->name) {
        name = inner->name;
        if (strcmp(inner->name, "Array") == 0)
            tid = 14;
        else if (strcmp(inner->name, "Map") == 0)
            tid = 16;
        else if (strcmp(inner->name, "Set") == 0)
            tid = 15;
        else if (strcmp(inner->name, "Json") == 0)
            tid = 18;
    }
    if (tid < 0 && inner->kind == XR_TREF_GENERIC && inner->name) {
        name = inner->name;
        if (strcmp(inner->name, "Array") == 0)
            tid = 14;
        else if (strcmp(inner->name, "Map") == 0)
            tid = 16;
        else if (strcmp(inner->name, "Set") == 0)
            tid = 15;
    }
    *out_tid = tid;
    *out_name = name;
}

static XiValue *lower_as_expr(XiLower *l, AstNode *node) {
    AsExprNode *as = &node->as.as_expr;
    XiValue *val = xi_lower_expr(l, as->expr);
    if (!val)
        return NULL;

    /* Resolve XrTypeRef kind to runtime XrTypeId.
     * AsExprNode.type is XrTypeRef*, not XrType*. */
    XrTypeRef *tref = as->type;
    struct XrType *source_type = xi_lower_node_type(l, as->expr);
    struct XrType *cast_type = xi_lower_node_type(l, node);
    if (!cast_type || XR_TYPE_IS_UNKNOWN(cast_type))
        cast_type = tref ? xr_tref_resolve(l->isolate, tref) : NULL;
    cast_type = xi_lower_type_or_any(l, cast_type, "cast target type", node->line);
    XrConversionWitness conversion = {0};
    bool has_conversion = xa_typed_program_conversion(l->typed_program, node, &conversion);
    if (!as->is_safe && cast_type && source_type && XR_TYPE_IS_NUMERIC(cast_type) &&
        XR_TYPE_IS_NUMERIC(source_type)) {
        if (!has_conversion || !xr_conversion_kind_is_numeric(conversion.kind) ||
            conversion.kind == XR_CONVERSION_DISALLOWED) {
            fprintf(stderr, "[LOWER] numeric `as` lacks a valid conversion witness at line %d\n",
                    (int) node->line);
            l->had_error = true;
            return NULL;
        }
        XiValue *result = NULL;
        if (conversion.kind == XR_CONVERSION_EXPLICIT_TARGET_WIDTH ||
            (XR_TYPE_IS_INT(cast_type) != XR_TYPE_IS_INT(source_type))) {
            result = xi_value_new(l->func, l->cur_block, XI_CONVERT, cast_type, 1);
            if (result)
                result->args[0] = val;
        } else if (XR_TYPE_IS_INT(cast_type) && XR_TYPE_IS_INT(source_type)) {
            result = xi_lower_narrow_for_static_type(l, node, val, cast_type);
        } else if (XR_TYPE_IS_FLOAT(cast_type) && XR_TYPE_IS_FLOAT(source_type)) {
            result = xi_lower_narrow_for_static_type(l, node, val, cast_type);
        }
        if (!result)
            return NULL;
        if (result == val) {
            result = xi_value_new(l->func, l->cur_block, XI_COPY, cast_type, 1);
            if (!result)
                return NULL;
            result->args[0] = val;
        }
        result->conversion = conversion;
        result->line = (uint32_t) node->line;
        if (XR_TYPE_IS_FLOAT(source_type) && XR_TYPE_IS_INT(cast_type))
            result->flags |= XI_FLAG_MAY_THROW;
        return result;
    }
    if (!as->is_safe && has_conversion && xr_conversion_kind_is_numeric(conversion.kind)) {
        fprintf(stderr,
                "[LOWER] unresolved numeric conversion '%s' must not reach dynamic XI_AS at line "
                "%d\n",
                xr_conversion_kind_name(conversion.kind), (int) node->line);
        l->had_error = true;
        return NULL;
    }
    /* Structural narrowing to a sealed Record is a checked conversion. A bare
     * XI_AS only compares the runtime type id, which every object-shaped value
     * shares, so the result would keep a foreign field layout while the static
     * type promises the target's — later field reads would address unverified
     * slots. Route both `as T` and `as T?` through the validated decode and
     * differ only in how a rejected value is reported. */
    if (cast_type && xi_type_is_checkable_record(cast_type) &&
        xi_type_may_carry_record_shape(source_type)) {
        XiValue *narrowed = lower_record_shape_narrow(l, val, cast_type, node->line);
        if (!narrowed)
            return NULL;
        if (as->is_safe)
            return narrowed;
        char msg[192];
        snprintf(msg, sizeof(msg), "E0404: value does not match the field set of '%s'",
                 xr_type_to_string(cast_type));
        return lower_null_guard_or_throw(l, narrowed, cast_type, arena_strdup(l->func, msg),
                                         node->line);
    }

    int tid;
    const char *tname;
    lower_dynamic_as_target(tref, &tid, &tname);

    bool is_safe = as->is_safe;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, l->type_any, 1);
    if (!v)
        return NULL;
    v->args[0] = val;
    /* Pack tid and is_safe into aux_int: bits[32:1]=tid as 32-bit two's
     * complement, bit[0]=is_safe. The intermediate uint32_t cast is
     * required because `tid` can be -1 (unrecognised generic name); a
     * signed left shift of a negative value is undefined behaviour and
     * would surface as a UBSan failure (linux-asan).
     * The corresponding decode in xi_emit_arith.c uses the signed shift
     * `aux_int >> 1` and reads the low 32 bits back into `int`, which
     * round-trips the sentinel on every two's-complement target. */
    v->aux_int = ((int64_t) (uint32_t) tid << 1) | (is_safe ? 1 : 0);
    v->aux = (void *) arena_strdup(l->func, tname);
    if (has_conversion)
        v->conversion = conversion;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_slice_expr(XiLower *l, AstNode *node) {
    SliceExprNode *sl = &node->as.slice_expr;
    XiSequenceEvidenceIds sequence_ids;
    uint8_t sequence_access_kind =
        lower_type_has_sequence_evidence(xi_lower_node_type(l, sl->source)) ? XG_SEQ_ACCESS_SLICE
                                                                            : 0;
    XiSequenceEvidenceKinds sequence_kinds = {
        .sequence_access_kind = sequence_access_kind,
    };
    xi_lower_take_sequence_evidence_ids(l, (uint32_t) node->line, sequence_kinds, &sequence_ids);
    XiValue *src = xi_lower_expr(l, sl->source);
    XiValue *start = sl->start ? xi_lower_expr(l, sl->start)
                               : xi_const_int(l->func, l->cur_block, 0, l->type_int);
    /* Omitted end clamps to the container length without colliding with
     * negative slice indices. */
    XiValue *end = sl->end ? xi_lower_expr(l, sl->end)
                           : xi_const_int(l->func, l->cur_block, INT64_MAX, l->type_int);
    if (!src)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_SLICE, result_type, 3);
    if (!v)
        return NULL;
    v->args[0] = src;
    v->args[1] = start;
    v->args[2] = end;
    v->line = (uint32_t) node->line;
    xi_lower_apply_sequence_evidence_ids(v, &sequence_ids);
    return v;
}

static XiValue *lower_range_expr(XiLower *l, AstNode *node) {
    RangeNode *rn = &node->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    if (!start || !end)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_RANGE, result_type, 2);
    if (!v)
        return NULL;
    v->args[0] = start;
    v->args[1] = end;
    v->aux_int = rn->inclusive_end ? 1 : 0;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_struct_literal(XiLower *l, AstNode *node) {
    StructLiteralNode *sl = &node->as.struct_literal;
    int count = sl->field_count;
    if (count < 0 || count > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] struct literal field count exceeds %d at line %d\n",
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }
    int n = count;

    /* Evaluate field values first */
    int alloc_n = n > 0 ? n : 1;
    XiValue **val_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!val_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, sl->field_values[i]);
        if (!val_vals[i])
            return NULL;
    }

    /* Resolve struct class from scope: local → shared → upvalue.
     * Struct declarations are lowered as XI_CLASS_CREATE and bound to
     * a variable with the struct name, so the lookup chain works the
     * same way as for class constructors in lower_new_expr. */
    const char *sname = sl->struct_name;
    XiValue *cls = NULL;
    if (sname) {
        int var_id = xi_lower_var_find(l, 0, sname);
        if (var_id >= 0) {
            if (l->is_program && l->shared_map[var_id] >= 0) {
                XiTopBinding b;
                b.slot = l->shared_map[var_id];
                b.name = l->vars[var_id].name;
                b.type = l->vars[var_id].type;
                cls = xi_lower_emit_top_load(l, b, l->type_any);
            } else {
                cls = xi_lower_braun_read(l, var_id, l->cur_block);
            }
        }
        if (!cls) {
            XiTopBinding tb = xi_lower_find_top_binding(l, 0, sname);
            if (xi_top_binding_valid(tb))
                cls = xi_lower_emit_top_load(l, tb, l->type_any);
        }
        if (!cls) {
            struct XrType *upval_type = NULL;
            int upval_idx = xi_lower_resolve_upvalue(l, 0, sname, &upval_type);
            if (upval_idx >= 0) {
                cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, l->type_any, 0);
                if (cls)
                    cls->aux_int = upval_idx;
            }
        }
    }

    struct XrType *result_type = xi_lower_node_type(l, node);

    /* Struct with layout: emit XI_AGG_NEW + XI_AGG_SET.
     * Emitter decides stack vs heap based on local use-scan. */
    if (cls) {
        XrAggregateLayout *slayout = xi_lower_type_struct_layout(l, result_type);
        if (!slayout)
            slayout = xi_lower_lookup_struct_layout(l, sname);

        if (slayout) {
            XiValue *inst = xi_value_new(l->func, l->cur_block, XI_AGG_NEW, result_type, 1);
            if (!inst)
                return NULL;
            inst->args[0] = cls;
            inst->aux = (void *) slayout;
            inst->flags |= XI_FLAG_SIDE_EFFECT;
            inst->line = (uint32_t) node->line;

            for (int i = 0; i < n; i++) {
                if (!val_vals[i] || !sl->field_names[i])
                    continue;
                int fidx = xi_lower_struct_field_index(slayout, sl->field_names[i]);
                if (fidx < 0)
                    continue;
                struct XrType *field_type =
                    xi_lower_struct_field_type(l, val_vals[i]->type, slayout, fidx);
                val_vals[i] = xi_lower_apply_numeric_conversion_witness(l, sl->field_values[i],
                                                                        val_vals[i], field_type);
                if (!val_vals[i])
                    return NULL;
                XiValue *field_val = xi_lower_narrow_for_native_field(
                    l, sl->field_values[i], val_vals[i], slayout->fields[fidx].native_type);
                XiValue *set = xi_value_new(l->func, l->cur_block, XI_AGG_SET, l->type_unit, 2);
                if (!set)
                    break;
                set->args[0] = inst;
                set->args[1] = field_val;
                set->aux = (void *) slayout;
                set->aux_int = fidx;
                set->flags |= XI_FLAG_SIDE_EFFECT;
            }
            return inst;
        }

        /* No layout (generic struct) → constructor call fallback */
        XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, 1);
        if (!call)
            return NULL;
        call->args[0] = cls;
        call->aux = (void *) "constructor";
        call->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
        call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        call->line = (uint32_t) node->line;

        for (int i = 0; i < n; i++) {
            if (!val_vals[i] || !sl->field_names[i])
                continue;
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, l->type_unit, 2);
            if (!set)
                break;
            set->args[0] = call;
            set->args[1] = val_vals[i];
            set->aux = (void *) arena_strdup(l->func, sl->field_names[i]);
            set->aux_int = xi_lower_method_symbol(l, sl->field_names[i]);
            set->flags |= XI_FLAG_SIDE_EFFECT;
        }
        return call;
    }

    fprintf(stderr, "[LOWER] unresolved struct literal '%s' at line %d\n",
            sname ? sname : "<anonymous>", (int) node->line);
    l->had_error = true;
    return NULL;
}

/*
 * Optional chain: obj?.name or obj?[idx]
 * Short-circuits to null if obj is null.
 */
static XiValue *lower_optional_chain(XiLower *l, AstNode *node) {
    OptionalChainNode *oc = &node->as.optional_chain;
    XiValue *obj = xi_lower_expr(l, oc->object);
    if (!obj)
        return NULL;

    /* Check if obj is null */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null)
        return obj;
    is_null->args[0] = obj;

    XiBlock *access_blk = xi_block_new(l->func);
    XiBlock *null_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, is_null, null_blk, access_blk);
    xi_lower_braun_seal(l, access_blk);
    xi_lower_braun_seal(l, null_blk);

    /* Null path → produce null */
    l->cur_block = null_blk;
    XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
    xi_block_set_jump(l->cur_block, merge);

    /* Access path → perform member access or index */
    l->cur_block = access_blk;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *access_val = NULL;
    if (oc->name) {
        /* Property access: obj.name */
        access_val = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
        if (access_val) {
            access_val->args[0] = obj;
            access_val->aux = (void *) arena_strdup(l->func, oc->name);
            access_val->aux_int = xi_lower_method_symbol(l, oc->name);
            /* Same class-field binding the non-optional `obj.name` path does.
             * The receiver is a `T?`, but nullability is a flag on XrType, not
             * a kind, so the instance payload needed to resolve the field is
             * still present; the null case never reaches here (null_blk). AOT
             * rejects a XI_LOAD_FIELD on a class with no field id bound. */
            xi_lower_bind_class_field_id(l, access_val, obj->type, oc->name);
        }
    } else if (oc->index) {
        /* Index access: obj[idx] */
        XiValue *idx = xi_lower_expr(l, oc->index);
        access_val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
        if (access_val) {
            access_val->args[0] = obj;
            access_val->args[1] = idx;
        }
    }
    XiBlock *access_exit = l->cur_block;
    xi_block_set_jump(access_exit, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;

    /* PHI merge: null or accessed value */
    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == null_blk)
                phi->value.args[i] = null_val;
            else
                phi->value.args[i] = access_val ? access_val : null_val;
        }
    }
    return phi ? &phi->value : null_val;
}

/* expr! — force unwrap nullable; runtime null-check then pass-through.
 * Throws Exception(E0413, "Attempted to unwrap a null value") on null. */
/* Guard `val` against null: on the null path raise `message` through the active
 * error channel, on the ok path yield the value typed as `result_type`. Shared
 * by force unwrap and by the validated structural narrowing below so both
 * failures reach the same channel with the same shape. */
static XiValue *lower_null_guard_or_throw(XiLower *l, XiValue *val, struct XrType *result_type,
                                          const char *message, int line) {
    XR_DCHECK(l != NULL, "null guard: NULL lowering context");
    XR_DCHECK(message != NULL, "null guard: NULL message");
    if (!val)
        return NULL;

    XiValue *chk = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!chk)
        return val;
    chk->args[0] = val;

    XiBlock *ok_blk = xi_block_new(l->func);
    XiBlock *throw_blk = xi_block_new(l->func);
    xi_block_set_if(l->cur_block, chk, throw_blk, ok_blk);
    xi_lower_braun_seal(l, throw_blk);
    xi_lower_braun_seal(l, ok_blk);

    /* Freestanding has no hosted Exception/unwind channel. A failed force
     * unwrap is a terminal panic hook/trap, matching other freestanding
     * runtime-error paths. */
    l->cur_block = throw_blk;
    if (l->analyzer && xa_analyzer_is_freestanding(l->analyzer)) {
        XiValue *panic_arg = xi_const_null(l->func, l->cur_block, l->type_null);
        XiValue *thr = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_unit, 1);
        if (thr) {
            thr->args[0] = panic_arg;
            thr->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
            thr->line = (uint32_t) line;
        }
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block->control = thr;
        l->cur_block = ok_blk;
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
        if (copy)
            copy->args[0] = val;
        return copy ? copy : val;
    }

    /* Throw path: construct Exception(E0413) and throw */
    struct XrType *exception_type = xr_type_new_class(NULL, "PanicInfo");
    XiValue *cls = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, exception_type, 0);
    if (!cls) {
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block = ok_blk;
        return val;
    }
    cls->aux_int = XR_GLOBAL_VAR_PANIC_INFO;
    cls->aux = (void *) "PanicInfo";

    XiValue *msg = xi_const_str(l->func, l->cur_block, message, l->type_string);
    XiValue *exc = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, exception_type, 2);
    if (!exc) {
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block = ok_blk;
        return val;
    }
    exc->args[0] = cls;
    exc->args[1] = msg;
    exc->aux = (void *) "constructor";
    exc->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
    exc->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    exc->line = (uint32_t) line;

    if (l->try_depth > 0) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_ERR_SET, l->type_unit, 1);
        if (set) {
            set->args[0] = exc;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) line;
        }
        xi_lower_defer_run_to_depth(l, l->catch_defer_depths[l->try_depth - 1], line);
        XiBlock *catch_blk = l->catch_targets[l->try_depth - 1];
        xi_block_set_jump(l->cur_block, catch_blk);
        l->cur_block = NULL;
    } else {
        XiValue *thr = xi_value_new(l->func, l->cur_block, XI_ERR_RETURN, l->type_unit, 1);
        if (thr) {
            thr->args[0] = exc;
            thr->flags |= XI_FLAG_SIDE_EFFECT;
            thr->line = (uint32_t) line;
        }
        l->cur_block->kind = XI_BLOCK_RETURN;
        l->cur_block->control = thr;
        l->cur_block = NULL;
    }

    /* Ok path */
    l->cur_block = ok_blk;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (copy)
        copy->args[0] = val;
    return copy ? copy : val;
}

static XiValue *lower_force_unwrap(XiLower *l, AstNode *node) {
    XiValue *val = xi_lower_expr(l, node->as.unary.operand);
    if (!val)
        return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *operand_type = xi_lower_node_type(l, node->as.unary.operand);
    bool operand_may_be_null = !operand_type || XR_TYPE_IS_UNKNOWN(operand_type) ||
                               operand_type->is_nullable || operand_type->kind == XR_KIND_NULL ||
                               xr_type_intrinsically_includes_null(operand_type);
    if (!operand_may_be_null) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
        if (copy)
            copy->args[0] = val;
        return copy ? copy : val;
    }
    return lower_null_guard_or_throw(l, val, result_type, "E0413: Attempted to unwrap a null value",
                                     node->line);
}

/* A sealed Record has a closed field set, so narrowing an object-shaped value
 * to one is a validated conversion, not a reinterpretation: field reads on the
 * result address slots by ordinal, and an unchecked source with a different
 * layout would hand back a value of the wrong type from a slot that was never
 * verified. The typed-decode path already confirms every declared field is
 * present and of the declared kind (recursively) and yields null otherwise, so
 * `is` and `as` both route through it and share one definition of the rule. */
static XiValue *lower_record_shape_narrow(XiLower *l, XiValue *val, struct XrType *record_type,
                                          int line) {
    XR_DCHECK(l != NULL, "record narrow: NULL lowering context");
    if (!val || !record_type)
        return NULL;
    int fc = record_type->object.field_count;
    if (fc <= 0 || !record_type->object.field_names)
        return NULL;

    const char **names =
        (const char **) xi_func_arena_alloc(l->func, (uint32_t) (fc * (int) sizeof(const char *)));
    if (!names)
        return NULL;
    for (int i = 0; i < fc; i++)
        names[i] = arena_strdup(l->func, record_type->object.field_names[i]);

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_DECODE, record_type, 1);
    if (!v)
        return NULL;
    v->args[0] = val;
    v->aux = (void *) names;
    v->aux_int = fc;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) line;
    return v;
}

/* True when `type` is a Record whose full field set is known at compile time,
 * i.e. the only form a runtime shape check can be built from. */
static bool xi_type_is_checkable_record(struct XrType *type) {
    return type && XR_TYPE_IS_RECORD(type) && type->object.is_sealed &&
           type->object.field_count > 0 && type->object.field_names != NULL;
}

/* Any source may be compared against a Record layout: the check answers false
 * for a value that carries no matching field set, which is exactly what a test
 * against an int or a class instance should report. A union of Records reaches
 * the check this way too. `string` is the one exclusion — the shared decode
 * path parses a string as JSON text, and a cast is not a parse request. */
static bool xi_type_may_carry_record_shape(struct XrType *type) {
    return !type || !XR_TYPE_IS_STRING(type);
}

static XiValue *lower_this_expr(XiLower *l, AstNode *node) {
    struct XrType *this_type = xi_lower_node_type(l, node);

    /* Try local scope first (direct method context) */
    int var_id = xi_lower_var_find(l, 0, "this");
    if (var_id >= 0 && l->vars[var_id].call_place) {
        XiValue *load = xi_value_new(l->func, l->cur_block, XI_PLACE_LOAD, l->vars[var_id].type, 1);
        if (!load)
            return NULL;
        load->args[0] = l->vars[var_id].call_place;
        load->var_id = (XiVarId) var_id;
        load->line = (uint32_t) node->line;
        return load;
    }
    if (var_id >= 0)
        return xi_lower_braun_read(l, var_id, l->cur_block);

    /* Not local — capture from enclosing method via upvalue */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, 0, "this", &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type)
            upval_type = this_type;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, upval_type, 0);
        if (v)
            v->aux_int = upval_idx;
        return v;
    }

    /* No 'this' in scope (e.g. top-level code) — return null */
    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *lower_super_call(XiLower *l, AstNode *node) {
    SuperCallNode *sc = &node->as.super_call;

    /* Resolve the receiver and the inherited ParamContract before constructing
     * the call. The analyzer owns that contract; lowering only consumes it to
     * materialize the same call-bound places as ordinary calls. */
    int var_id = xi_lower_var_find(l, 0, "this");
    if (var_id < 0)
        var_id = xi_lower_var_create(l, 0, "this", l->type_any);
    struct XrType *this_type = var_id >= 0 ? l->vars[var_id].type : l->type_any;
    XiValue *this_val = var_id >= 0 ? xi_lower_braun_read(l, var_id, l->cur_block) : NULL;

    const char *method_name = sc->method_name ? sc->method_name : XR_KEYWORD_CONSTRUCTOR;
    const char *class_name = xr_type_get_class_name(this_type);
    XrClassInfo *class_info = class_name ? xi_lower_lookup_class_info(l, class_name) : NULL;
    XrClassInfo *base_info = class_info ? class_info->base : NULL;
    XaSymbol *method_sym = base_info ? xa_class_info_lookup_member(base_info, method_name) : NULL;
    XaSymbolLinks *method_links =
        (method_sym && l->analyzer) ? xa_analyzer_get_links(l->analyzer, method_sym) : NULL;
    struct XrType *method_type = method_links ? method_links->type : NULL;
    if (!method_type || method_type->kind != XR_KIND_FUNCTION)
        method_type = NULL;
    XrParamMode stack_method_modes[64];
    const XrParamMode *method_modes = NULL;
    int method_pcount = 0;
    if (method_type) {
        method_modes = lower_function_param_modes(
            l, method_type, stack_method_modes,
            (int) (sizeof(stack_method_modes) / sizeof(stack_method_modes[0])), &method_pcount);
    }

    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    for (int i = 0; i < sc->arg_count; i++) {
        XiValue *arg = xi_lower_expr(l, sc->arguments[i]);
        if (!arg)
            return NULL;
        if (!xi_lower_arg_list_push(l, &args, arg, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
    }
    XiValue **arg_vals = args.items;
    int n = args.count;

    CallExprNode call_view = {
        .arguments = sc->arguments,
        .arg_accesses = sc->arg_accesses,
        .arg_count = sc->arg_count,
    };
    XiCallWriteback *writebacks = NULL;
    XiCallPlan *call_plan =
        lower_build_call_plan(l, &call_view, arg_vals, n, method_modes, method_pcount, NULL, 0,
                              method_type, &writebacks, (int) node->line);
    if (l->had_error)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    uint16_t nargs = (uint16_t) (n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
    if (!call)
        return NULL;
    call->args[0] = this_val ? this_val : xi_const_null(l->func, l->cur_block, l->type_null);
    for (int i = 0; i < n; i++)
        call->args[i + 1] = arg_vals[i];
    call->aux = (void *) arena_strdup(l->func, method_name);
    call->aux_int = ((int64_t) xi_lower_method_symbol(l, method_name) << 1) | 1;
    call->call_plan = call_plan;
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    call->line = (uint32_t) node->line;
    xi_lower_bind_callsite_id(l, call, xi_lower_source_node_id(l, node));
    xi_lower_insert_err_check(l, node, true);
    if (!lower_apply_call_writebacks(l, call_plan, writebacks, (int) node->line))
        return NULL;
    return call;
}

/* Enum access/decl/convert, object literal, catch, cancelled, move
 * are now in xi_lower_misc.c */

/* Main expression dispatcher */
XR_FUNC XiValue *xi_lower_expr(XiLower *l, AstNode *node) {
    if (!node)
        return NULL;
    if (!l->cur_block)
        return NULL; /* dead code after return/break */

    /* Canonicalizer value block: `{ var __t = recv; <expr using __t> }`.  The
     * canonicalizer emits this when a place expression must be evaluated
     * exactly once (spec §3.0 E6) and the receiver is not simple.  Every
     * statement but the last is lowered as a statement; the last one produces
     * the block's value.  Only canonicalizer-generated blocks are values, so a
     * user block reaching expression position still falls to the switch and is
     * reported as the compiler bug it is. */
    if (node->type == AST_BLOCK && node->as.block.is_canon_value_block) {
        int count = node->as.block.count;
        XR_DCHECK(count > 0, "canon value block must have a value statement");
        for (int i = 0; i + 1 < count; i++) {
            xi_lower_stmt(l, node->as.block.statements[i]);
            if (!l->cur_block)
                return NULL;
        }
        return xi_lower_expr(l, node->as.block.statements[count - 1]);
    }

    switch (node->type) {
        /* Literals */
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_NULL:
        case AST_LITERAL_STRING:
            return lower_literal(l, node);
        case AST_FIXED_BYTES_LITERAL:
            return lower_fixed_bytes_literal(l, node);

        /* Binary operations */
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            return lower_binary(l, node);

        /* Unary */
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            return lower_unary(l, node);

        /* Grouping: just unwrap */
        case AST_GROUPING:
            return xi_lower_expr(l, node->as.grouping);
        case AST_COMPTIME_EXPR:
            if (node->as.comptime_expr.expr) {
                XrCtValue value = {0};
                const char *err = NULL;
                if (xa_analyzer_get_node_ct_value(l->analyzer, node, &value) ||
                    xa_consteval_expr(l->analyzer, node, &value, &err)) {
                    XiValue *lowered = lower_ct_value(l, node, &value, xi_lower_node_type(l, node));
                    if (lowered)
                        return lowered;
                }
                if (err) {
                    fprintf(stderr,
                            "[LOWER] comptime expression was not consteval-safe at line "
                            "%d: %s\n",
                            (int) node->line, err);
                }
                l->had_error = true;
                return xi_const_null(l->func, l->cur_block, l->type_null);
            }
            l->had_error = true;
            return xi_const_null(l->func, l->cur_block, l->type_null);

        /* Variables and assignment */
        case AST_VARIABLE:
            return lower_variable(l, node);
        case AST_ASSIGNMENT:
            return lower_assignment(l, node);
        case AST_COMPOUND_ASSIGNMENT:
        case AST_INC:
        case AST_DEC:
            /* Canonicalized away: compound assignment → plain assignment,
             * inc/dec → assignment with +1/-1. Must never reach here. */
            XR_DCHECK(false, "AST_COMPOUND_ASSIGNMENT / INC / DEC "
                             "must be canonicalized before lowering");
            l->had_error = true;
            return NULL;

        /* Calls */
        case AST_CALL_EXPR:
            return lower_call(l, node);

        /* Ternary */
        case AST_TERNARY:
            return lower_ternary(l, node);

        /* Member / index access */
        case AST_MEMBER_ACCESS:
            return lower_member_access(l, node);
        case AST_MEMBER_SET:
            return lower_member_set(l, node);
        case AST_INDEX_GET:
            return lower_index_get(l, node);
        case AST_INDEX_SET:
            return lower_index_set(l, node);
        case AST_ARRAY_LITERAL:
            return lower_array_literal(l, node);
        case AST_TUPLE_LITERAL:
            return lower_tuple_literal(l, node);
        case AST_MAP_LITERAL:
            return lower_map_literal(l, node);

        case AST_OBJECT_LITERAL:
            return xi_lower_object_literal(l, node);

        /* Nullish coalesce */
        case AST_NULLISH_COALESCE:
            return lower_nullish_coalesce(l, node);

        /* Match expression */
        case AST_MATCH_EXPR:
            return xi_lower_match(l, node);

        /* Function / closure */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return xi_lower_function_decl(l, node);

        /* Object creation */
        case AST_NEW_EXPR:
            return lower_new_expr(l, node);

        /* Coroutine */
        case AST_GO_EXPR:
            return lower_go_expr(l, node);
        case AST_AWAIT_EXPR:
            return lower_await_expr(l, node);
        case AST_UNSAFE_EXPR:
            /* Transparent: unsafe only constrains analysis, not codegen. The
             * body is a statement block; its trailing expression statement is
             * the value (or null when the block ends with a non-expression). */
            return lower_unsafe_expr(l, node);
        case AST_CHANNEL_NEW:
            return lower_channel_new(l, node);
        /* Template string / set literal */
        case AST_TEMPLATE_STRING:
            return lower_template_string(l, node);
        case AST_SET_LITERAL:
            return lower_set_literal(l, node);

        /* Type operations */
        case AST_IS_EXPR:
            return lower_is_expr(l, node);
        case AST_AS_EXPR:
            return lower_as_expr(l, node);

        /* Slice / range */
        case AST_SLICE_EXPR:
            return lower_slice_expr(l, node);
        case AST_RANGE:
            return lower_range_expr(l, node);

        /* Struct literal / optional chain */
        case AST_STRUCT_LITERAL:
            return lower_struct_literal(l, node);
        case AST_OPTIONAL_CHAIN:
            return lower_optional_chain(l, node);

        /* Force unwrap: expr! */
        case AST_FORCE_UNWRAP:
            return lower_force_unwrap(l, node);

        /* OOP: this / super */
        case AST_THIS_EXPR:
            return lower_this_expr(l, node);
        case AST_SUPER_CALL:
            return lower_super_call(l, node);

        /* Enum access / index */
        case AST_ENUM_ACCESS:
            return xi_lower_enum_access(l, node);
        case AST_ENUM_INDEX:
            return xi_lower_enum_access(l, node); /* same pattern: load field */

        case AST_CANCELLED_EXPR:
            return xi_lower_cancelled_expr(l, node);
        case AST_MOVE_EXPR:
            return xi_lower_move_expr(l, node);

        /* Scope blocks are statement-only; keep a defensive null for old ASTs. */
        case AST_SCOPE_BLOCK: {
            (void) xi_lower_scope_block(l, node);
            return xi_const_null(l->func, l->cur_block, l->type_null);
        }

        /* BigInt: lowered as a BigInt constant (string digits + BigInt type) */
        case AST_LITERAL_BIGINT:
            return xi_const_bigint(
                l->func, l->cur_block,
                node->as.literal.raw_value.bigint_val ? node->as.literal.raw_value.bigint_val : "0",
                l->type_bigint);
        case AST_LITERAL_REGEX: {
            const char *pattern = node->as.literal.raw_value.regex.pattern;
            const char *flags = node->as.literal.raw_value.regex.flags;
            XiValue *pat_v =
                xi_const_str(l->func, l->cur_block, pattern ? pattern : "", l->type_string);
            XiValue *flg_v =
                xi_const_str(l->func, l->cur_block, flags ? flags : "", l->type_string);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_REGEX_COMPILE, l->type_regex, 2);
            if (v) {
                v->args[0] = pat_v;
                v->args[1] = flg_v;
            }
            return v;
        }

        /* Expression statement wrapper: unwrap */
        case AST_EXPR_STMT:
            return xi_lower_expr(l, node->as.expr_stmt);

        default:
            /* Every analyzer-accepted AST node must be lowerable.
             * Reaching here indicates a compiler bug, not a user error. */
            XR_DCHECK_FMT(false, "unsupported expr AST kind %d in lowering", (int) node->type);
            l->had_error = true;
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

/* Class declaration lowering (method compilation + XI_CLASS_CREATE).
 * Factored into .inc.c to keep individual files under the 3000-line limit. */
#include "xi_lower_class.inc.c"
