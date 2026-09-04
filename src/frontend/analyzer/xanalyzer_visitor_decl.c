/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_decl.c - Pass 1 collect helpers for declarations,
 *                            Pass 1.5 class-inheritance linking, and
 *                            return-type inference scanner
 *
 * KEY CONCEPT:
 *   Holds the bulk of "declaration-shaped" analyzer code that used to
 *   crowd xanalyzer_visitor.c past the 2500-line mark:
 *
 *     - collect_return_types / xa_infer_function_return_type
 *         (post-hoc return-type inference for unannotated functions)
 *
 *     - xa_visit_collect_function_decl_only / _function_body /
 *       xa_visit_collect_function (two-phase function symbol collect
 *       supporting mutual recursion via hoisting)
 *
 *     - contains_this_expr / stmt_contains_this
 *         (constructor super() validation: no `this` access before
 *          super() returns)
 *
 *     - xa_visit_collect_class
 *         (class symbol creation, field / method / generic param
 *          registration, struct layout)
 *
 *     - xa_visit_collect_var_decl
 *         (top-level var/const/shared symbol)
 *
 *     - build_class_vtable / xa_link_class_inheritance
 *         (Pass 1.5 entry point: resolve base class names to
 *          XrClassInfo pointers and build vtables)
 *
 *   This file holds the declaration-shaped subset of the analyzer
 *   visitor. The two collect helpers reachable from the hoisting loop
 *   in xanalyzer_visitor.c are non-static so they can be called
 *   cross-TU; see xanalyzer_visitor_internal.h.
 */

#include "xanalyzer_visitor_internal.h"
#include "xanalyzer_builtin_interfaces.h"
#include "xa_intrinsic_registry.h"
#include "xtype_ref_resolve.h"
#include "../parser/xtype_ref.h"
#include "../../base/xchecks.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xstruct_layout.h"
#include <limits.h>
#include "../../module/xmodule_graph.h"
#include "../../module/xnative_package.h"
#include "../../toolchain/xcompiler_session.h"

static void xa_publish_deprecated_attrs(XaSymbolLinks *links, XrAttribute **attrs, int count) {
    for (int i = 0; links && attrs && i < count; i++) {
        if (attrs[i] && attrs[i]->kind == ATTR_DEPRECATED) {
            xa_symbol_links_set_deprecated(links, true, attrs[i]->str_arg);
            return;
        }
    }
    xa_symbol_links_set_deprecated(links, false, NULL);
}

static XrType *xa_normalize_structural_constraint(XaInferContext *ctx, XrType *constraint) {
    if (!ctx || !ctx->analyzer || !constraint || !XR_TYPE_IS_STRUCT_OBJECT(constraint))
        return constraint;
    XrType *normalized = xr_type_new_struct_object_with_fields(
        ctx->analyzer->isolate, constraint->object.field_names, constraint->object.field_types,
        constraint->object.field_count);
    if (!normalized)
        return constraint;
    if (constraint->object.type_name)
        xr_type_set_object_type_name(ctx->analyzer->isolate, normalized,
                                     constraint->object.type_name);
    return normalized;
}

// Store `<T, U: A & B>` generic params, with every intersection-style
// constraint resolved to a runtime XrType, on the declaration's symbol links.
// Shared by functions, classes and methods so a method's own constraints are
// tracked exactly like a top-level function's.
static void xa_store_type_params_with_constraints(XaInferContext *ctx, XaSymbolLinks *links,
                                                  XrGenericParam **type_params, int count,
                                                  AstNode *node) {
    if (!links || !type_params || count <= 0)
        return;

    const char **names = xr_malloc(sizeof(const char *) * count);
    XrType ***constraint_lists = xr_malloc(sizeof(XrType **) * count);
    int *constraint_counts = xr_malloc(sizeof(int) * count);

    if (names && constraint_lists && constraint_counts) {
        for (int i = 0; i < count; i++) {
            XrGenericParam *gp = type_params[i];
            names[i] = gp ? gp->name : NULL;

            int cn = gp ? gp->constraint_count : 0;
            if (cn > 0 && gp->constraints) {
                XrType **resolved = xr_malloc(sizeof(XrType *) * cn);
                for (int j = 0; j < cn; j++) {
                    // Use analyzer-aware resolver so class-bounded constraints
                    // (e.g. <T: Animal>) keep their inheritance chain.
                    resolved[j] =
                        gp->constraints[j]
                            ? xr_tref_resolve_in_analyzer(ctx->analyzer, gp->constraints[j])
                            : NULL;
                    if (gp->constraints[j] && gp->constraints[j]->kind == XR_TREF_OBJECT &&
                        !gp->constraints[j]->name && gp->constraints[j]->field_readonly) {
                        for (uint8_t k = 0; k < gp->constraints[j]->nchildren; k++) {
                            if (!gp->constraints[j]->field_readonly[k])
                                continue;
                            XrLocation loc = {.file = ctx->file_path,
                                              .line = gp->constraints[j]->line,
                                              .column = gp->constraints[j]->column};
                            xa_analyzer_add_diagnostic(
                                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                "structural generic constraints cannot declare const fields; "
                                "field writability is inferred from the generic body",
                                &loc);
                            break;
                        }
                    }
                    resolved[j] = xa_normalize_structural_constraint(ctx, resolved[j]);
                    xa_reject_error_type_success_type(ctx->analyzer, resolved[j],
                                                      "generic constraint", gp->name, node->line,
                                                      node->column);
                }
                constraint_lists[i] = resolved;
                constraint_counts[i] = cn;
            } else {
                constraint_lists[i] = NULL;
                constraint_counts[i] = 0;
            }
        }

        xa_symbol_links_set_type_params(links, names, constraint_lists, constraint_counts, count);

        // set_type_params deep-copies constraint arrays — release temporaries.
        for (int i = 0; i < count; i++) {
            if (constraint_lists[i])
                xr_free(constraint_lists[i]);
        }
    }

    xr_free(names);
    xr_free(constraint_lists);
    xr_free(constraint_counts);
}

static int xa_fixed_array_elem_native_lane(XrType *elem) {
    if (!elem || elem->is_nullable)
        return XR_NATIVE_VALUE;
    int native = xr_type_kind_to_native(elem->kind, elem->scalar_rep);
    if (native == XR_NATIVE_I64 || native == XR_NATIVE_F64 || native == XR_NATIVE_BOOL ||
        native == XR_NATIVE_I8 || native == XR_NATIVE_I16 || native == XR_NATIVE_I32 ||
        native == XR_NATIVE_U8 || native == XR_NATIVE_U16 || native == XR_NATIVE_U32 ||
        native == XR_NATIVE_U64 || native == XR_NATIVE_ISIZE || native == XR_NATIVE_USIZE ||
        native == XR_NATIVE_F32)
        return native;
    return XR_NATIVE_VALUE;
}

static const char *xa_intrinsic_owner_module(const XaInferContext *ctx) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    return ctx->analyzer->current_module_is_stdlib ? ctx->analyzer->current_stdlib_module_name
                                                   : NULL;
}

static void xa_bind_registry_intrinsic(XaInferContext *ctx, AstNode *node, XaSymbol *symbol,
                                       const char *owner_name, const char *member_name,
                                       bool is_static, int arity) {
    const char *canonical_module = xa_intrinsic_owner_module(ctx);
    if (!canonical_module || !member_name)
        return;
    char key[256];
    int key_len = owner_name ? snprintf(key, sizeof(key), "%s.%s.%s", canonical_module, owner_name,
                                        member_name)
                             : snprintf(key, sizeof(key), "%s.%s", canonical_module, member_name);
    if (key_len <= 0 || (size_t) key_len >= sizeof(key))
        return;
    const XaIntrinsicDesc *desc = xa_intrinsic_by_key(key);
    if (!desc)
        return;
    XrLocation loc = {.file = ctx ? ctx->file_path : NULL,
                      .line = node ? node->line : 0,
                      .column = node ? node->column : 0};
    bool expects_static = (desc->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) != 0;
    if (expects_static != is_static || arity < desc->min_arity || arity > desc->max_arity) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "compiler-owned declaration signature disagrees with the "
                                   "canonical intrinsic registry",
                                   &loc);
        return;
    }
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    if (links)
        links->intrinsic_id = desc->id;
}

static void xa_bind_param_default_exprs(XaInferContext *ctx, AstNode **defaults,
                                        XrType **param_types, int count) {
    if (!ctx || !defaults || count <= 0)
        return;
    XrType *saved_expected = ctx->expected_type;
    for (int i = 0; i < count; i++) {
        if (defaults[i]) {
            ctx->expected_type = param_types ? param_types[i] : NULL;
            xa_visit_infer_expr(ctx, defaults[i]);

            /*
             * Export metadata must not retain a dependency on a private
             * declaration-module const: the VM imports that metadata into a
             * separate analyzer where the private symbol intentionally does
             * not exist.  Publish scalar consteval defaults as self-contained
             * literals.  Dynamic defaults remain caller-evaluated expressions.
             */
            XrCtValue value = {0};
            const char *ct_error = NULL;
            if (xa_consteval_expr(ctx->analyzer, defaults[i], &value, &ct_error)) {
                AstNodeType literal_type = AST_LITERAL_NULL;
                bool can_fold = true;
                switch (value.kind) {
                    case XR_CT_INT:
                        literal_type = AST_LITERAL_INT;
                        break;
                    case XR_CT_FLOAT:
                        literal_type = AST_LITERAL_FLOAT;
                        break;
                    case XR_CT_BOOL:
                        literal_type = value.as.bool_val ? AST_LITERAL_TRUE : AST_LITERAL_FALSE;
                        break;
                    case XR_CT_STRING:
                        literal_type = AST_LITERAL_STRING;
                        break;
                    case XR_CT_CHAR:
                        literal_type = AST_LITERAL_RUNE;
                        break;
                    case XR_CT_NULL:
                        literal_type = AST_LITERAL_NULL;
                        break;
                    default:
                        can_fold = false;
                        break;
                }
                if (can_fold) {
                    AstNode *folded = defaults[i];
                    memset(&folded->as, 0, sizeof(folded->as));
                    folded->type = literal_type;
                    folded->as.literal.escape_mode = XR_LITERAL_ESCAPED;
                    folded->as.literal.source_form = XR_LITERAL_INLINE;
                    switch (value.kind) {
                        case XR_CT_INT:
                            folded->as.literal.kind = LITERAL_KIND_INT;
                            folded->as.literal.int_bits = (uint64_t) value.as.int_val;
                            folded->as.literal.raw_value.int_val = value.as.int_val;
                            break;
                        case XR_CT_FLOAT:
                            folded->as.literal.kind = LITERAL_KIND_FLOAT;
                            folded->as.literal.raw_value.float_val = value.as.float_val;
                            break;
                        case XR_CT_BOOL:
                            folded->as.literal.kind = LITERAL_KIND_BOOL;
                            folded->as.literal.raw_value.bool_val = value.as.bool_val;
                            break;
                        case XR_CT_STRING:
                            folded->as.literal.kind = LITERAL_KIND_STRING;
                            folded->as.literal.raw_value.string_val = value.as.string_val;
                            break;
                        case XR_CT_CHAR:
                            folded->as.literal.kind = LITERAL_KIND_RUNE;
                            folded->as.literal.raw_value.rune_val = value.as.rune_val;
                            break;
                        case XR_CT_NULL:
                            folded->as.literal.kind = LITERAL_KIND_NULL;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
    ctx->expected_type = saved_expected;
}

static bool xa_c_export_native_scalar_supported(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_F64:
        case XR_NATIVE_BOOL:
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
        case XR_NATIVE_POINTER:
        case XR_NATIVE_F32:
            return true;
        default:
            return false;
    }
}

static bool xa_native_lane_bitwise_reinterpretable(uint8_t native_type) {
    return xa_c_export_native_scalar_supported(native_type);
}

static bool xa_struct_layout_bitwise_reinterpretable_depth(const XrAggregateLayout *layout,
                                                           int depth) {
    if (!layout || depth > 8)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->is_flexible)
            return false;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            if (!xa_struct_layout_bitwise_reinterpretable_depth(field->sub_layout, depth + 1))
                return false;
            continue;
        }
        if (field->native_type == XR_NATIVE_ARRAY) {
            if (!xa_native_lane_bitwise_reinterpretable(field->elem_native_type))
                return false;
            continue;
        }
        if (!xa_native_lane_bitwise_reinterpretable(field->native_type))
            return false;
    }
    return true;
}

/* Can a slot of this type be `weak`?
 *
 * Only something with a reference-counted identity: `weak` works by holding a
 * handle instead of the value and clearing it when the target's last strong
 * reference goes. A scalar has no refcount and no death, so `weak` on one would
 * silently mean nothing — which is worse than rejecting it. A struct is a value
 * type inlined into its holder and likewise has no identity to weaken. */
static bool xa_type_can_be_weak(const XrType *type) {
    if (!type || type->is_value_type)
        return false;
    switch (type->kind) {
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_STRING:
        case XR_KIND_FUNCTION:
        case XR_KIND_JSON:
            return true;
        default:
            return false;
    }
}

/* Does an instance of this type carry a `weak` field, its bases included? */
bool xa_type_declares_weak_field(const XrType *type) {
    if (!type)
        return false;
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return false;
    for (XrClassInfo *c = type->instance.class_ref; c; c = c->base) {
        for (int i = 0; i < c->field_count; i++) {
            XaSymbol *f = c->fields[i];
            if (f && f->is_weak)
                return true;
        }
    }
    return false;
}

/* W4 (spec 16.3): `weak` is only meaningful in the EXEC_LOCAL domain.
 *
 * Clearing a weak slot (W5) runs off the owning coroutine's heap when the
 * target's last strong reference goes. An object in a shared or module-static
 * domain outlives — or is reachable outside — that heap, so there is no single
 * death point to hang the clearing on: the slot would keep reading a target
 * that some other execution context already released. Rejecting it at the
 * declaration is the only place the user can still choose a different design.
 *
 * TRANSFERABLE is forbidden as well.  The weak table belongs to one execution
 * heap; moving the holder would leave its handle in the old heap. */
static void xa_check_weak_storage_domain(XaInferContext *ctx, const XrType *type, uint8_t domain,
                                         const XrLocation *loc) {
    if (domain == XR_STORAGE_EXEC_LOCAL || domain == XR_STORAGE_DOMAIN_UNKNOWN)
        return;
    if (!xa_type_declares_weak_field(type))
        return;
    const char *domain_label = domain == XR_STORAGE_MODULE_STATIC  ? "module-static"
                               : domain == XR_STORAGE_CONST_SHARED ? "const-shared"
                               : domain == XR_STORAGE_SYNC_SHARED  ? "sync-shared"
                               : domain == XR_STORAGE_TRANSFERABLE ? "transferable"
                                                                   : "non-local";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "a type with a weak field cannot live in %s storage: nothing there would clear the "
             "slot when its target dies",
             domain_label);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WEAK_FIELD, msg,
                               loc);
}

static bool xa_struct_field_bitwise_reinterpretable(const XrAggregateFieldLayout *field) {
    if (!field)
        return false;
    if (field->is_flexible)
        return false;
    if (field->native_type == XR_NATIVE_NESTED_AGGREGATE)
        return xa_struct_layout_bitwise_reinterpretable_depth(field->sub_layout, 0);
    if (field->native_type == XR_NATIVE_ARRAY)
        return xa_native_lane_bitwise_reinterpretable(field->elem_native_type);
    return xa_native_lane_bitwise_reinterpretable(field->native_type);
}

static void xa_validate_extern_cfn_callback_param_modes(XaInferContext *ctx, AstNode *node,
                                                        const XrParamNode *param,
                                                        const XrType *type, bool is_return) {
    if (!ctx || !ctx->analyzer || !type || !XR_TYPE_IS_C_FUNCTION(type))
        return;
    for (int i = 0; i < type->function.param_count; i++) {
        XrParamMode callback_mode = xr_type_function_param_mode(type, i);
        if (callback_mode == XR_PARAM_READ)
            continue;
        XrLocation loc = {.file = ctx->file_path,
                          .line = param ? param->line : (node ? node->line : 0),
                          .column = param ? param->column : (node ? node->column : 0)};
        char msg[320];
        if (is_return) {
            snprintf(msg, sizeof(msg),
                     "extern CFn return uses unsupported callback parameter mode '%s' at callback "
                     "parameter %d before verified extern callback ABI wrapper contract",
                     xr_param_mode_label(callback_mode), i + 1);
        } else {
            snprintf(msg, sizeof(msg),
                     "extern CFn parameter '%s' uses unsupported callback parameter mode '%s' at "
                     "callback parameter %d before verified extern callback ABI wrapper contract",
                     param && param->name ? param->name : "?", xr_param_mode_label(callback_mode),
                     i + 1);
        }
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }
}

/* C ABI boundary types: bool, exact integers (including usize/isize), f32 and
 * f64, Ptr<T>/MutPtr<T>, plus fixed-layout value structs bound through the
 * native manifest. Every other kind is a managed representation whose pointer
 * is meaningless to C -- passing one compiles today and then silently reads
 * object headers on the other side, so the declaration is rejected here. */
static bool xa_extern_type_is_boundary(const XrType *type) {
    if (!type)
        return true; /* unresolved: the type checker owns this case */
    switch (type->kind) {
        case XR_KIND_INT: /* every exact width, plus usize/isize */
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_POINTER:
            return true;
        case XR_KIND_UNKNOWN:
        case XR_KIND_ERROR:
            return true; /* error recovery already diagnosed the cause */
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return xa_type_has_fixed_layout_data_object(type);
        default:
            return false;
    }
}

static void xa_report_extern_non_boundary(XaInferContext *ctx, XrLocation *loc, bool is_return,
                                          const char *name, XrType *type) {
    char msg[384];
    const char *site = is_return ? "extern function return type" : "extern function parameter";
    if (type && type->kind == XR_KIND_STRING) {
        snprintf(msg, sizeof(msg),
                 "%s '%s' uses 'string', which is a managed heap object, not a C string; pass an "
                 "explicit UTF-8 byte view (Ptr<u8>/MutPtr<u8> plus a length) instead",
                 site, name ? name : "?");
    } else {
        snprintf(msg, sizeof(msg),
                 "%s '%s' uses type '%s', which is not C-ABI-representable; extern boundaries "
                 "accept bool, exact integers, f32/f64, usize/isize, Ptr<T>/MutPtr<T>, CFn<...> "
                 "and fixed-layout value structs",
                 site, name ? name : "?", xr_type_to_string(type));
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg, loc);
}

static void xa_validate_extern_function_abi(XaInferContext *ctx, AstNode *node,
                                            const FunctionDeclNode *fn, XrType **param_types,
                                            XrType *return_type) {
    if (!ctx || !ctx->analyzer || !fn)
        return;
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        XrParamMode mode = param ? param->passing_mode : XR_PARAM_READ;
        if (mode != XR_PARAM_READ) {
            /* Fail closed until task-190/task-206 define verified extern ParamMode ABI wrappers. */
            XrLocation loc = {.file = ctx->file_path,
                              .line = param ? param->line : (node ? node->line : 0),
                              .column = param ? param->column : (node ? node->column : 0)};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "extern function parameter '%s' uses unsupported parameter mode '%s' before "
                     "verified extern ABI contract",
                     param && param->name ? param->name : "?", xr_param_mode_label(mode));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        }
        XrType *type = param_types ? param_types[i] : NULL;
        if (xr_type_is_enum_metadata(type)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = param ? param->line : (node ? node->line : 0),
                              .column = param ? param->column : (node ? node->column : 0)};
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "extern function parameter '%s' uses enum descriptor type '%s', which has "
                     "no stable C ABI; pass explicit scalar schema data instead",
                     param && param->name ? param->name : "?", xr_type_to_string(type));
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        } else if (type && XR_TYPE_IS_C_FUNCTION(type)) {
            xa_validate_extern_cfn_callback_param_modes(ctx, node, param, type, false);
        } else if (type && XR_TYPE_IS_FUNCTION(type)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = param ? param->line : (node ? node->line : 0),
                              .column = param ? param->column : (node ? node->column : 0)};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "extern function parameter '%s' uses Xray function type; use CFn<...> for C "
                     "function pointers",
                     param && param->name ? param->name : "?");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       msg, &loc);
        } else if (type && !xa_extern_type_is_boundary(type)) {
            XrLocation loc = {.file = ctx->file_path,
                              .line = param ? param->line : (node ? node->line : 0),
                              .column = param ? param->column : (node ? node->column : 0)};
            xa_report_extern_non_boundary(ctx, &loc, false,
                                          param && param->name ? param->name : "?", type);
        }
    }
    if (xr_type_is_enum_metadata(return_type)) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = node ? node->line : 0,
                          .column = node ? node->column : 0};
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "extern function returns enum descriptor type '%s', which has no stable C ABI; "
                 "return explicit scalar schema data instead",
                 xr_type_to_string(return_type));
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    } else if (return_type && XR_TYPE_IS_C_FUNCTION(return_type))
        xa_validate_extern_cfn_callback_param_modes(ctx, node, NULL, return_type, true);
    if (return_type && XR_TYPE_IS_FUNCTION(return_type) && !XR_TYPE_IS_C_FUNCTION(return_type)) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = node ? node->line : 0,
                          .column = node ? node->column : 0};
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "extern function returns Xray function type; use CFn<...> for C function pointers",
            &loc);
    } else if (return_type && return_type->kind != XR_KIND_UNIT &&
               !XR_TYPE_IS_C_FUNCTION(return_type) && !xa_extern_type_is_boundary(return_type)) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = node ? node->line : 0,
                          .column = node ? node->column : 0};
        xa_report_extern_non_boundary(ctx, &loc, true, fn->name, return_type);
    }
}

static bool xa_expr_is_this(AstNode *node) {
    if (!node)
        return false;
    if (node->type == AST_THIS_EXPR)
        return true;
    return node->type == AST_VARIABLE && node->as.variable.name &&
           strcmp(node->as.variable.name, "this") == 0;
}

typedef struct XaParamEscapeSummary {
    XrType **param_types;
    const char **param_names;
    int param_count;
    XaParamEffectSummary *effects;
    XrType *return_type;
    const char *aliases[128];
    int alias_slot[128];
    int alias_count;
    XaInferContext *ctx;
    XrClassInfo *receiver_info;
    const XrViewOrigin *declared_view_origins;
    int declared_view_origin_count;
    bool validate_view_origins;
} XaParamEscapeSummary;

static XaSymbolLinks *xa_summary_function_links(XaParamEscapeSummary *summary, AstNode *callee);
static XaSymbolLinks *xa_summary_receiver_method_links(XaParamEscapeSummary *summary,
                                                       AstNode *callee);
static XrType *xa_summary_expr_type(XaParamEscapeSummary *summary, AstNode *node);

static int xa_summary_param_slot(XaParamEscapeSummary *summary, const char *name) {
    if (!summary || !name)
        return -1;
    for (int i = summary->alias_count - 1; i >= 0; i--) {
        if (summary->aliases[i] && strcmp(summary->aliases[i], name) == 0)
            return summary->alias_slot[i];
    }
    for (int i = 0; i < summary->param_count; i++) {
        if (summary->param_names && summary->param_names[i] &&
            strcmp(summary->param_names[i], name) == 0)
            return i;
    }
    return -1;
}

static int xa_summary_declared_param_slot(XaParamEscapeSummary *summary, const char *name) {
    if (!summary || !name)
        return -1;
    /* A nested function parameter or local declaration with the same name
     * masks the outer formal. Alias entries are ordered by lexical walk, so
     * the newest matching entry decides before consulting declared formals. */
    for (int i = summary->alias_count - 1; i >= 0; i--) {
        if (summary->aliases[i] && strcmp(summary->aliases[i], name) == 0)
            return -1;
    }
    for (int i = 0; i < summary->param_count; i++) {
        if (summary->param_names && summary->param_names[i] &&
            strcmp(summary->param_names[i], name) == 0)
            return i;
    }
    return -1;
}

static void xa_summary_set_alias(XaParamEscapeSummary *summary, const char *name, int slot) {
    if (!summary || !name)
        return;
    for (int i = summary->alias_count - 1; i >= 0; i--) {
        if (summary->aliases[i] && strcmp(summary->aliases[i], name) == 0) {
            summary->alias_slot[i] = slot;
            return;
        }
    }
    if (summary->alias_count >= 128)
        return;
    summary->aliases[summary->alias_count] = name;
    summary->alias_slot[summary->alias_count] = slot;
    summary->alias_count++;
}

static bool xa_summary_name_is_local(XaParamEscapeSummary *summary, const char *name) {
    if (!summary || !name)
        return false;
    for (int i = summary->alias_count - 1; i >= 0; i--) {
        if (summary->aliases[i] && strcmp(summary->aliases[i], name) == 0)
            return true;
    }
    for (int i = 0; i < summary->param_count; i++) {
        if (summary->param_names && summary->param_names[i] &&
            strcmp(summary->param_names[i], name) == 0)
            return true;
    }
    return false;
}

static XrType *xa_summary_known_expr_type(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr)
        return NULL;
    XrType *type = summary->ctx ? xa_analyzer_get_node_type(summary->ctx->analyzer, expr) : NULL;
    if (type)
        return type;
    if (expr->type == AST_VARIABLE) {
        int slot = xa_summary_param_slot(summary, expr->as.variable.name);
        if (slot >= 0 && slot < summary->param_count && summary->param_types)
            return summary->param_types[slot];
        return NULL;
    }
    if (expr->type == AST_INDEX_GET) {
        XrType *container = xa_summary_known_expr_type(summary, expr->as.index_get.array);
        return (XrType *) xr_type_contiguous_element_type(container);
    }
    return NULL;
}

static const XrViewOrigin *xa_summary_view_return_contract(XaParamEscapeSummary *summary,
                                                           AstNode *callee, XaSymbolLinks *links,
                                                           int *out_count) {
    if (out_count)
        *out_count = 0;
    if (links && links->return_view.origin_count > 0 && links->return_view.origins) {
        if (out_count)
            *out_count = links->return_view.origin_count;
        return links->return_view.origins;
    }
    XrType *type = xa_summary_expr_type(summary, callee);
    if (!type || !XR_TYPE_IS_FUNCTION(type) || type->function.view_origin_count <= 0 ||
        !type->function.view_origin_set)
        return NULL;
    if (out_count)
        *out_count = type->function.view_origin_count;
    return type->function.view_origin_set;
}

static int xa_summary_expr_root_param_slot(XaParamEscapeSummary *summary, AstNode *expr) {
    while (expr) {
        XrType *expr_type = xa_summary_known_expr_type(summary, expr);
        if (expr_type && !xa_type_needs_borrow_escape_guard(expr_type) &&
            !XR_TYPE_IS_POINTER(expr_type))
            return -1;
        switch (expr->type) {
            case AST_VARIABLE:
                return xa_summary_param_slot(summary, expr->as.variable.name);
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                break;
            case AST_OPTIONAL_CHAIN:
                expr = expr->as.optional_chain.object;
                break;
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_MOVE_EXPR:
                expr = expr->as.move_expr.expr;
                break;
            case AST_CALL_EXPR: {
                CallExprNode *call = &expr->as.call_expr;
                /* The builtin copy(x) yields a fresh owner, so its result roots
                 * at no parameter. Only the one-argument form is that builtin:
                 * copy is not a reserved word, and a user-declared copy of any
                 * other arity may well return one of its arguments. */
                if (call->arg_count == 1 && call->callee && call->callee->type == AST_VARIABLE &&
                    call->callee->as.variable.name &&
                    strcmp(call->callee->as.variable.name, "copy") == 0)
                    return -1;
                XaSymbolLinks *callee = xa_summary_function_links(summary, call->callee);
                if (!callee)
                    callee = xa_summary_receiver_method_links(summary, call->callee);
                int origin_count = 0;
                const XrViewOrigin *origins =
                    xa_summary_view_return_contract(summary, call->callee, callee, &origin_count);
                if (!origins || origin_count != 1)
                    return -1;
                if (origins[0].kind == XR_VIEW_ORIGIN_PARAM && origins[0].param_ordinal >= 0 &&
                    origins[0].param_ordinal < call->arg_count && call->arguments) {
                    expr = call->arguments[origins[0].param_ordinal];
                    break;
                }
                if (origins[0].kind == XR_VIEW_ORIGIN_RECEIVER && call->callee &&
                    call->callee->type == AST_MEMBER_ACCESS) {
                    expr = call->callee->as.member_access.object;
                    break;
                }
                return -1;
            }
            default:
                return -1;
        }
    }
    return -1;
}

/* Mutation provenance is meaningful for scalar ref parameters as well as
 * borrowed heap/view values. Keep this separate from the escape-root helper:
 * escape tracking deliberately ignores non-borrowing scalars, while a write
 * through `ref int` must still be part of the canonical parameter effect. */
static int xa_summary_mutation_root_param_slot(XaParamEscapeSummary *summary, AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
                return xa_summary_param_slot(summary, expr->as.variable.name);
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                break;
            case AST_OPTIONAL_CHAIN:
                expr = expr->as.optional_chain.object;
                break;
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                break;
            default:
                return -1;
        }
    }
    return -1;
}

static bool xa_summary_method_stores_argument(const char *method_name, int slot);
static bool xa_summary_call_requires_owned_move_argument(XaParamEscapeSummary *summary,
                                                         AstNode *callee, int slot);
static bool xa_summary_member_call_requires_writable_receiver(XaParamEscapeSummary *summary,
                                                              AstNode *callee);
static XaSymbolLinks *xa_summary_function_links(XaParamEscapeSummary *summary, AstNode *callee);
static void xa_summary_mark_unknown_function_value_args(XaParamEscapeSummary *summary,
                                                        CallExprNode *call);
static XrClassInfo *xa_type_class_info(XrType *type);
static XrClassInfo *xa_summary_type_class_info(XaParamEscapeSummary *summary, XrType *type);
static void xa_summary_mark_expr(XaParamEscapeSummary *summary, AstNode *expr);
static void xa_summary_mark_return(XaParamEscapeSummary *summary, AstNode *expr);
static void xa_summary_mark_mutation(XaParamEscapeSummary *summary, AstNode *expr);
static void xa_summary_mark_owned_move_requirement(XaParamEscapeSummary *summary, AstNode *expr);
static void xa_summary_walk(XaParamEscapeSummary *summary, AstNode *node);

static XrClassInfo *xa_summary_type_class_info(XaParamEscapeSummary *summary, XrType *type) {
    XrClassInfo *info = xa_type_class_info(type);
    if (info || !summary || !summary->ctx || !summary->ctx->analyzer || !type)
        return info;
    const char *class_name =
        XR_TYPE_IS_INTERFACE(type) ? type->instance.class_name : xr_type_get_class_name(type);
    if (!class_name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(summary->ctx->analyzer->current_scope, class_name);
    if (!sym)
        sym = xa_scope_lookup(summary->ctx->analyzer->global_scope, class_name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(summary->ctx->analyzer, sym) : NULL;
    return links ? links->class_info : NULL;
}

static XrClassInfo *xa_summary_expr_class_info(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return NULL;
    if (xa_expr_is_this(node))
        return summary->receiver_info;
    switch (node->type) {
        case AST_VARIABLE: {
            int slot = xa_summary_param_slot(summary, node->as.variable.name);
            if (slot < 0 || slot >= summary->param_count || !summary->param_types)
                return NULL;
            return xa_summary_type_class_info(summary, summary->param_types[slot]);
        }
        case AST_MEMBER_ACCESS: {
            XrClassInfo *owner_info =
                xa_summary_expr_class_info(summary, node->as.member_access.object);
            if (!owner_info)
                return NULL;
            XaSymbol *member = xa_class_info_lookup_member(owner_info, node->as.member_access.name);
            if (!member || (member->kind != XA_SYM_FIELD && member->kind != XA_SYM_PROPERTY))
                return NULL;
            return xa_summary_type_class_info(summary, member->links.type);
        }
        case AST_GROUPING:
            return xa_summary_expr_class_info(summary, node->as.grouping);
        case AST_FORCE_UNWRAP:
            return xa_summary_expr_class_info(summary, node->as.unary.operand);
        case AST_MOVE_EXPR:
            return xa_summary_expr_class_info(summary, node->as.move_expr.expr);
        default:
            return NULL;
    }
}

static XaSymbolLinks *xa_summary_receiver_method_links(XaParamEscapeSummary *summary,
                                                       AstNode *callee) {
    if (!summary || !summary->ctx || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    XrClassInfo *target_info = xa_summary_expr_class_info(summary, ma->object);
    XaSymbol *method_sym = target_info ? xa_class_info_lookup_member(target_info, ma->name) : NULL;
    if (!method_sym || method_sym->kind != XA_SYM_METHOD)
        return NULL;
    return method_sym ? xa_analyzer_get_links(summary->ctx->analyzer, method_sym) : NULL;
}

static void xa_summary_mark_call_expr(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr || expr->type != AST_CALL_EXPR)
        return;
    CallExprNode *call = &expr->as.call_expr;
    /* The builtin copy(x) hands back a fresh owner, so nothing the caller
     * passed can escape through it. Any other arity is a user-declared
     * function that happens to be named copy and must be summarised
     * normally. */
    if (call->arg_count == 1 && call->callee && call->callee->type == AST_VARIABLE &&
        call->callee->as.variable.name && strcmp(call->callee->as.variable.name, "copy") == 0)
        return;
    XaSymbolLinks *fn_links = xa_summary_function_links(summary, call->callee);
    if (fn_links && fn_links->param_effects) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_effect_count; i++) {
            if (xa_param_effect_retains_or_escapes(&fn_links->param_effects[i]))
                xa_summary_mark_expr(summary, call->arguments[i]);
        }
    }
    if (fn_links && fn_links->param_effects) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_effect_count; i++) {
            if (xa_param_effect_mutates(&fn_links->param_effects[i]))
                xa_summary_mark_mutation(summary, call->arguments[i]);
        }
    }
    if (fn_links && fn_links->param_effects) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_effect_count; i++) {
            if (fn_links->param_effects[i].storage_domain == XR_STORAGE_TRANSFERABLE)
                xa_summary_mark_owned_move_requirement(summary, call->arguments[i]);
        }
    }
    if (!fn_links)
        xa_summary_mark_unknown_function_value_args(summary, call);
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        const char *method_name = call->callee->as.member_access.name;
        if (xa_summary_member_call_requires_writable_receiver(summary, call->callee))
            xa_summary_mark_mutation(summary, call->callee->as.member_access.object);
        for (int i = 0; i < call->arg_count; i++) {
            if (xa_summary_method_stores_argument(method_name, i))
                xa_summary_mark_expr(summary, call->arguments[i]);
            if (xa_summary_call_requires_owned_move_argument(summary, call->callee, i))
                xa_summary_mark_owned_move_requirement(summary, call->arguments[i]);
        }
        XaSymbolLinks *method_links = xa_summary_receiver_method_links(summary, call->callee);
        if (method_links && method_links->param_effects) {
            for (int i = 0; i < call->arg_count && i < method_links->param_effect_count; i++) {
                if (xa_param_effect_retains_or_escapes(&method_links->param_effects[i]))
                    xa_summary_mark_expr(summary, call->arguments[i]);
            }
        }
        if (method_links && method_links->param_effects) {
            for (int i = 0; i < call->arg_count && i < method_links->param_effect_count; i++) {
                if (xa_param_effect_mutates(&method_links->param_effects[i]))
                    xa_summary_mark_mutation(summary, call->arguments[i]);
            }
        }
        if (method_links && method_links->param_effects) {
            for (int i = 0; i < call->arg_count && i < method_links->param_effect_count; i++) {
                if (method_links->param_effects[i].storage_domain == XR_STORAGE_TRANSFERABLE)
                    xa_summary_mark_owned_move_requirement(summary, call->arguments[i]);
            }
        }
    }
}

static void xa_summary_mark_expr(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr)
        return;
    XrType *expr_type = xa_summary_known_expr_type(summary, expr);
    if (expr_type && !xa_type_needs_borrow_escape_guard(expr_type) &&
        !XR_TYPE_IS_POINTER(expr_type))
        return;
    int slot = xa_summary_expr_root_param_slot(summary, expr);
    if (slot >= 0 && slot < summary->param_count) {
        summary->effects[slot].retain = XA_RETAIN_LOCAL_ALIAS;
        summary->effects[slot].escapes |= XA_ESCAPE_LOCAL_STORAGE;
    }
    switch (expr->type) {
        case AST_CALL_EXPR:
            xa_summary_mark_call_expr(summary, expr);
            break;
        case AST_GROUPING:
            xa_summary_mark_expr(summary, expr->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_summary_mark_expr(summary, expr->as.unary.operand);
            break;
        case AST_MOVE_EXPR:
            xa_summary_mark_expr(summary, expr->as.move_expr.expr);
            break;
        case AST_SLICE_EXPR:
            xa_summary_mark_expr(summary, expr->as.slice_expr.source);
            break;
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
        case AST_NULLISH_COALESCE:
            xa_summary_mark_expr(summary, expr->as.binary.left);
            xa_summary_mark_expr(summary, expr->as.binary.right);
            break;
        case AST_TERNARY:
            xa_summary_mark_expr(summary, expr->as.ternary.true_expr);
            xa_summary_mark_expr(summary, expr->as.ternary.false_expr);
            break;
        case AST_ARRAY_LITERAL:
            if (expr->as.array_literal.is_repeat) {
                xa_summary_mark_expr(summary, expr->as.array_literal.repeat_value);
                xa_summary_mark_expr(summary, expr->as.array_literal.repeat_count);
            } else {
                for (int i = 0; i < expr->as.array_literal.count; i++)
                    xa_summary_mark_expr(summary, expr->as.array_literal.elements[i]);
            }
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < expr->as.object_literal.count; i++)
                xa_summary_mark_expr(summary, expr->as.object_literal.values[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < expr->as.map_literal.count; i++) {
                xa_summary_mark_expr(summary, expr->as.map_literal.keys[i]);
                xa_summary_mark_expr(summary, expr->as.map_literal.values[i]);
            }
            break;
        default:
            break;
    }
}

static void xa_summary_mark_mutation(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !summary->effects || !expr)
        return;
    int slot = xa_summary_mutation_root_param_slot(summary, expr);
    if (slot >= 0 && slot < summary->param_count) {
        summary->effects[slot].access |= XA_PARAM_ACCESS_WRITE;
        summary->effects[slot].mutation_paths |= XA_MUTATION_PATH_WILDCARD;
    }
}

static bool xa_summary_origin_append(XrViewOrigin *origins, int *count, int capacity,
                                     XrViewOrigin origin) {
    if (!origins || !count || *count < 0 || *count > capacity)
        return false;
    for (int i = 0; i < *count; i++) {
        if (origins[i].kind == origin.kind && origins[i].param_ordinal == origin.param_ordinal)
            return true;
    }
    if (*count >= capacity)
        return false;
    origins[(*count)++] = origin;
    return true;
}

static int xa_summary_collect_return_origins(XaParamEscapeSummary *summary, AstNode *expr,
                                             XrViewOrigin *origins, int capacity) {
    if (!summary || !expr || !origins || capacity <= 0)
        return -1;
    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        XaSymbolLinks *callee = xa_summary_function_links(summary, call->callee);
        if (!callee)
            callee = xa_summary_receiver_method_links(summary, call->callee);
        int contract_count = 0;
        const XrViewOrigin *contract =
            xa_summary_view_return_contract(summary, call->callee, callee, &contract_count);
        if (!contract || contract_count <= 0)
            return -1;
        int count = 0;
        for (int i = 0; i < contract_count; i++) {
            if (contract[i].kind == XR_VIEW_ORIGIN_STATIC) {
                if (!xa_summary_origin_append(
                        origins, &count, capacity,
                        (XrViewOrigin) {.kind = XR_VIEW_ORIGIN_STATIC, .param_ordinal = -1}))
                    return -1;
                continue;
            }
            AstNode *source = NULL;
            if (contract[i].kind == XR_VIEW_ORIGIN_PARAM && contract[i].param_ordinal >= 0 &&
                contract[i].param_ordinal < call->arg_count && call->arguments) {
                source = call->arguments[contract[i].param_ordinal];
            } else if (contract[i].kind == XR_VIEW_ORIGIN_RECEIVER && call->callee &&
                       call->callee->type == AST_MEMBER_ACCESS) {
                source = call->callee->as.member_access.object;
            }
            if (!source)
                return -1;
            XrViewOrigin nested[130];
            int nested_count = xa_summary_collect_return_origins(
                summary, source, nested, (int) (sizeof(nested) / sizeof(nested[0])));
            if (nested_count <= 0)
                return -1;
            for (int j = 0; j < nested_count; j++) {
                if (!xa_summary_origin_append(origins, &count, capacity, nested[j]))
                    return -1;
            }
        }
        return count;
    }

    int slot = xa_summary_expr_root_param_slot(summary, expr);
    if (slot >= 0 && slot < summary->param_count) {
        origins[0] = (XrViewOrigin) {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = (int16_t) slot};
        return 1;
    }
    AstNode *root = expr;
    while (root && (root->type == AST_MEMBER_ACCESS || root->type == AST_INDEX_GET ||
                    root->type == AST_SLICE_EXPR || root->type == AST_GROUPING ||
                    root->type == AST_FORCE_UNWRAP || root->type == AST_AS_EXPR)) {
        if (root->type == AST_MEMBER_ACCESS)
            root = root->as.member_access.object;
        else if (root->type == AST_INDEX_GET)
            root = root->as.index_get.array;
        else if (root->type == AST_SLICE_EXPR)
            root = root->as.slice_expr.source;
        else if (root->type == AST_GROUPING)
            root = root->as.grouping;
        else if (root->type == AST_AS_EXPR)
            root = root->as.as_expr.expr;
        else
            root = root->as.unary.operand;
    }
    if (xa_expr_is_this(root)) {
        origins[0] = (XrViewOrigin) {.kind = XR_VIEW_ORIGIN_RECEIVER, .param_ordinal = -1};
        return 1;
    }
    if (root && root->type == AST_VARIABLE && summary->ctx) {
        XaSymbol *symbol =
            root->as.variable.symbol_id
                ? xa_analyzer_symbol_by_id(summary->ctx->analyzer, root->as.variable.symbol_id)
                : xa_scope_lookup(summary->ctx->analyzer->current_scope, root->as.variable.name);
        XaSymbolLinks *links =
            symbol ? xa_analyzer_get_links(summary->ctx->analyzer, symbol) : NULL;
        if (symbol && symbol->is_const && links &&
            links->storage_domain == XR_STORAGE_MODULE_STATIC) {
            origins[0] = (XrViewOrigin) {.kind = XR_VIEW_ORIGIN_STATIC, .param_ordinal = -1};
            return 1;
        }
    }
    return -1;
}

static bool xa_summary_declares_origin(const XaParamEscapeSummary *summary, XrViewOrigin origin) {
    if (!summary)
        return false;
    for (int i = 0; i < summary->declared_view_origin_count; i++) {
        const XrViewOrigin *declared = &summary->declared_view_origins[i];
        if (declared->kind == origin.kind && declared->param_ordinal == origin.param_ordinal)
            return true;
    }
    return false;
}

static void xa_summary_mark_return(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr)
        return;
    XrViewOrigin actual[130];
    int actual_count = xa_summary_collect_return_origins(
        summary, expr, actual, (int) (sizeof(actual) / sizeof(actual[0])));
    if (actual_count <= 0 && summary->validate_view_origins &&
        summary->declared_view_origin_count > 0) {
        XrLocation loc = {.file = summary->ctx ? summary->ctx->file_path : NULL,
                          .line = expr->line,
                          .column = expr->column};
        xa_analyzer_add_diagnostic(
            summary->ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_BORROW_SOURCE,
            "OWN-E-VIEW-ORIGIN-INVALID: return path uses local, temporary, or unknown storage",
            &loc);
    }
    for (int i = 0; i < actual_count; i++) {
        if (summary->validate_view_origins && summary->declared_view_origin_count > 0 &&
            !xa_summary_declares_origin(summary, actual[i])) {
            XrLocation loc = {.file = summary->ctx ? summary->ctx->file_path : NULL,
                              .line = expr->line,
                              .column = expr->column};
            xa_analyzer_add_diagnostic(
                summary->ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_BORROW_SOURCE,
                "OWN-E-VIEW-ORIGIN-INVALID: return path origin is outside the declared 'from' set",
                &loc);
            continue;
        }
        if (summary->effects && actual[i].kind == XR_VIEW_ORIGIN_PARAM &&
            actual[i].param_ordinal >= 0 && actual[i].param_ordinal < summary->param_count) {
            int slot = actual[i].param_ordinal;
            summary->effects[slot].returns |= expr->type == AST_SLICE_EXPR
                                                  ? XA_RETURN_PROVENANCE_BORROWED_PROJECTION
                                                  : XA_RETURN_PROVENANCE_ALIAS;
            summary->effects[slot].retain = XA_RETAIN_LOCAL_ALIAS;
        }
    }
    xa_summary_mark_expr(summary, expr);
}

static void xa_summary_mark_owned_move_requirement(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr || expr->type != AST_MOVE_EXPR || !summary->effects)
        return;
    int slot = xa_summary_expr_root_param_slot(summary, expr->as.move_expr.expr);
    if (slot < 0 || slot >= summary->param_count)
        return;
    XrType *param_type = summary->param_types ? summary->param_types[slot] : NULL;
    if (!xa_boundary_transfer_type_needs_explicit(param_type))
        return;
    summary->effects[slot].storage_domain = XR_STORAGE_TRANSFERABLE;
}

static bool xa_summary_method_stores_argument(const char *method_name, int slot) {
    if (!method_name || slot < 0)
        return false;
    if ((strcmp(method_name, "push") == 0 || strcmp(method_name, "unshift") == 0 ||
         strcmp(method_name, "fill") == 0 || strcmp(method_name, "add") == 0 ||
         strcmp(method_name, "send") == 0 || strcmp(method_name, "trySend") == 0 ||
         strcmp(method_name, "sendTimeout") == 0) &&
        slot == 0)
        return true;
    return strcmp(method_name, "set") == 0 && (slot == 0 || slot == 1);
}

static XrType *xa_summary_expr_type(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return NULL;
    switch (node->type) {
        case AST_VARIABLE: {
            int slot = xa_summary_param_slot(summary, node->as.variable.name);
            if (slot >= 0 && slot < summary->param_count && summary->param_types)
                return summary->param_types[slot];
            XaSymbol *sym = summary->ctx
                                ? xa_lookup_visible_symbol(summary->ctx, node->as.variable.name)
                                : NULL;
            XaSymbolLinks *links = sym ? xa_analyzer_get_links(summary->ctx->analyzer, sym) : NULL;
            return links ? links->type : NULL;
        }
        case AST_MEMBER_ACCESS: {
            XrClassInfo *owner_info =
                xa_summary_expr_class_info(summary, node->as.member_access.object);
            if (!owner_info)
                return NULL;
            XaSymbol *member = xa_class_info_lookup_member(owner_info, node->as.member_access.name);
            return member ? member->links.type : NULL;
        }
        case AST_GROUPING:
            return xa_summary_expr_type(summary, node->as.grouping);
        case AST_FORCE_UNWRAP:
            return xa_summary_expr_type(summary, node->as.unary.operand);
        case AST_MOVE_EXPR:
            return xa_summary_expr_type(summary, node->as.move_expr.expr);
        case AST_AS_EXPR:
            return xa_summary_expr_type(summary, node->as.as_expr.expr);
        default:
            return NULL;
    }
}

static bool xa_summary_member_call_requires_writable_receiver(XaParamEscapeSummary *summary,
                                                              AstNode *callee) {
    if (!summary || !callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || !ma->object)
        return false;

    XrType *receiver_type = xa_summary_expr_type(summary, ma->object);
    if (receiver_type && XR_TYPE_IS_STRING(receiver_type))
        return false;

    XrClassInfo *target_info = xa_summary_expr_class_info(summary, ma->object);
    XaSymbol *method_sym = target_info ? xa_class_info_lookup_member(target_info, ma->name) : NULL;
    if (method_sym && method_sym->kind == XA_SYM_METHOD)
        return method_sym->receiver_mode != XR_PARAM_READ;

    return receiver_type &&
           xa_builtin_member_receiver_mode(receiver_type, ma->name) != XR_PARAM_READ;
}

static bool xa_summary_call_requires_owned_move_argument(XaParamEscapeSummary *summary,
                                                         AstNode *callee, int slot) {
    if (!summary || !callee || callee->type != AST_MEMBER_ACCESS || slot != 0)
        return false;
    const char *method_name = callee->as.member_access.name;
    XrType *receiver_type = xa_summary_expr_type(summary, callee->as.member_access.object);
    if (!receiver_type || receiver_type->kind != XR_KIND_CHANNEL)
        return false;
    return strcmp(method_name, "send") == 0 || strcmp(method_name, "trySend") == 0 ||
           strcmp(method_name, "sendTimeout") == 0;
}

static XaSymbolLinks *xa_summary_function_links(XaParamEscapeSummary *summary, AstNode *callee) {
    if (!summary || !summary->ctx || !callee || callee->type != AST_VARIABLE ||
        !callee->as.variable.name)
        return NULL;
    XaSymbol *sym =
        xa_scope_lookup(summary->ctx->analyzer->current_scope, callee->as.variable.name);
    if (!sym && summary->ctx->analyzer->global_scope)
        sym = xa_scope_lookup(summary->ctx->analyzer->global_scope, callee->as.variable.name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(summary->ctx->analyzer, sym);
    if (sym->kind == XA_SYM_FUNCTION)
        return links;
    if (sym->is_const && links && links->type && XR_TYPE_IS_FUNCTION(links->type) &&
        links->param_effects)
        return links;
    return NULL;
}

static void xa_summary_mark_unknown_function_value_args(XaParamEscapeSummary *summary,
                                                        CallExprNode *call) {
    if (!summary || !call || !call->callee)
        return;
    if (xa_summary_function_links(summary, call->callee))
        return;
    XrType *callee_type = xa_summary_expr_type(summary, call->callee);
    if (!callee_type || !XR_TYPE_IS_FUNCTION(callee_type))
        return;
    int view_origin_count = 0;
    const XrViewOrigin *view_origins =
        xa_summary_view_return_contract(summary, call->callee, NULL, &view_origin_count);
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        XrCallArgAccess access = call->arg_accesses ? call->arg_accesses[i] : XR_CALL_ARG_PLAIN;
        if (access == XR_CALL_ARG_REF) {
            /* The callable type authorizes a write, but a dynamic value has no
             * body summary. Record the possible mutation and keep the effect
             * incomplete so advisory diagnostics fail closed. */
            int root_slot = xa_summary_mutation_root_param_slot(summary, arg);
            if (root_slot >= 0 && root_slot < summary->param_count) {
                summary->effects[root_slot].complete = false;
                summary->effects[root_slot].incomplete_reason |= XA_UNKNOWN_DYNAMIC_CALL_TARGET;
            }
            xa_summary_mark_mutation(summary, arg);
            continue;
        }
        XrType *arg_type = xa_summary_expr_type(summary, arg);
        if (!xa_type_needs_borrow_escape_guard(arg_type) &&
            !(arg_type && XR_TYPE_IS_POINTER(arg_type)))
            continue;
        /* A complete single-source function type is a borrow-return contract, not an
         * arbitrary escape. Slice values cannot otherwise be retained by safe code, so the
         * designated source remains a caller-scoped alias and is accounted for by the return
         * expression itself. */
        bool is_return_origin = false;
        for (int origin_index = 0; origin_index < view_origin_count; origin_index++) {
            if (view_origins[origin_index].kind == XR_VIEW_ORIGIN_PARAM &&
                view_origins[origin_index].param_ordinal == i) {
                is_return_origin = true;
                break;
            }
        }
        if (is_return_origin && xa_type_contains_span_view(arg_type))
            continue;
        int root_slot = xa_summary_expr_root_param_slot(summary, arg);
        if (root_slot >= 0 && root_slot < summary->param_count) {
            summary->effects[root_slot].complete = false;
            summary->effects[root_slot].incomplete_reason |= XA_UNKNOWN_DYNAMIC_CALL_TARGET;
        }
        xa_summary_mark_expr(summary, arg);
        xa_summary_mark_mutation(summary, arg);
    }
}

static void xa_summary_mark_capture_refs(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return;
    int slot = xa_summary_expr_root_param_slot(summary, node);
    if (slot >= 0 && slot < summary->param_count) {
        summary->effects[slot].retain = XA_RETAIN_LOCAL_ALIAS;
        summary->effects[slot].escapes |= XA_ESCAPE_CLOSURE;
    }

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                xa_summary_mark_capture_refs(summary, node->as.block.statements[i]);
            break;
        case AST_EXPR_STMT:
            xa_summary_mark_capture_refs(summary, node->as.expr_stmt);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            xa_summary_mark_capture_refs(summary, node->as.var_decl.initializer);
            xa_summary_set_alias(summary, node->as.var_decl.name, -1);
            break;
        case AST_ASSIGNMENT:
            xa_summary_mark_capture_refs(summary, node->as.assignment.value);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                xa_summary_mark_capture_refs(summary, node->as.return_stmt.values[i]);
            break;
        case AST_CALL_EXPR:
            xa_summary_mark_capture_refs(summary, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                xa_summary_mark_capture_refs(summary, node->as.call_expr.arguments[i]);
            break;
        case AST_MEMBER_ACCESS:
            xa_summary_mark_capture_refs(summary, node->as.member_access.object);
            break;
        case AST_MEMBER_SET:
            xa_summary_mark_capture_refs(summary, node->as.member_set.object);
            xa_summary_mark_capture_refs(summary, node->as.member_set.value);
            break;
        case AST_INDEX_GET:
            xa_summary_mark_capture_refs(summary, node->as.index_get.array);
            xa_summary_mark_capture_refs(summary, node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            xa_summary_mark_capture_refs(summary, node->as.index_set.array);
            xa_summary_mark_capture_refs(summary, node->as.index_set.index);
            xa_summary_mark_capture_refs(summary, node->as.index_set.value);
            break;
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                xa_summary_mark_capture_refs(summary, node->as.array_literal.repeat_value);
                xa_summary_mark_capture_refs(summary, node->as.array_literal.repeat_count);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    xa_summary_mark_capture_refs(summary, node->as.array_literal.elements[i]);
            }
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++)
                xa_summary_mark_capture_refs(summary, node->as.object_literal.values[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                xa_summary_mark_capture_refs(summary, node->as.map_literal.keys[i]);
                xa_summary_mark_capture_refs(summary, node->as.map_literal.values[i]);
            }
            break;
        case AST_IF_STMT:
            xa_summary_mark_capture_refs(summary, node->as.if_stmt.condition);
            xa_summary_mark_capture_refs(summary, node->as.if_stmt.then_branch);
            xa_summary_mark_capture_refs(summary, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            xa_summary_mark_capture_refs(summary, node->as.while_stmt.condition);
            xa_summary_mark_capture_refs(summary, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.initializer);
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.condition);
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.increment);
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.body);
            break;
        case AST_FOR_IN_STMT:
            xa_summary_mark_capture_refs(summary, node->as.for_in_stmt.collection);
            xa_summary_mark_capture_refs(summary, node->as.for_in_stmt.body);
            break;
        case AST_FUNCTION_EXPR:
            break;
        case AST_GROUPING:
            xa_summary_mark_capture_refs(summary, node->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_summary_mark_capture_refs(summary, node->as.unary.operand);
            break;
        case AST_MOVE_EXPR:
            xa_summary_mark_capture_refs(summary, node->as.move_expr.expr);
            break;
        case AST_SLICE_EXPR:
            xa_summary_mark_capture_refs(summary, node->as.slice_expr.source);
            xa_summary_mark_capture_refs(summary, node->as.slice_expr.start);
            xa_summary_mark_capture_refs(summary, node->as.slice_expr.end);
            break;
        case AST_TERNARY:
            xa_summary_mark_capture_refs(summary, node->as.ternary.condition);
            xa_summary_mark_capture_refs(summary, node->as.ternary.true_expr);
            xa_summary_mark_capture_refs(summary, node->as.ternary.false_expr);
            break;
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
        case AST_NULLISH_COALESCE:
            xa_summary_mark_capture_refs(summary, node->as.binary.left);
            xa_summary_mark_capture_refs(summary, node->as.binary.right);
            break;
        default:
            break;
    }
}

static void xa_summary_walk_call(XaParamEscapeSummary *summary, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        const char *method_name = call->callee->as.member_access.name;
        if (xa_summary_member_call_requires_writable_receiver(summary, call->callee))
            xa_summary_mark_mutation(summary, call->callee->as.member_access.object);
        for (int i = 0; i < call->arg_count; i++) {
            AstNode *arg = call->arguments[i];
            if (xa_summary_method_stores_argument(method_name, i))
                xa_summary_mark_expr(summary, arg);
            if (xa_summary_call_requires_owned_move_argument(summary, call->callee, i))
                xa_summary_mark_owned_move_requirement(summary, arg);
            xa_summary_walk(summary, arg);
        }
        XaSymbolLinks *method_links = xa_summary_receiver_method_links(summary, call->callee);
        if (method_links && method_links->param_effects) {
            for (int i = 0; i < call->arg_count && i < method_links->param_effect_count; i++) {
                if (xa_param_effect_retains_or_escapes(&method_links->param_effects[i]))
                    xa_summary_mark_expr(summary, call->arguments[i]);
            }
        }
        if (method_links && method_links->param_effects) {
            for (int i = 0; i < call->arg_count && i < method_links->param_effect_count; i++) {
                if (xa_param_effect_mutates(&method_links->param_effects[i]))
                    xa_summary_mark_mutation(summary, call->arguments[i]);
            }
        }
        if (method_links && method_links->param_effects) {
            for (int i = 0; i < call->arg_count && i < method_links->param_effect_count; i++) {
                if (method_links->param_effects[i].storage_domain == XR_STORAGE_TRANSFERABLE)
                    xa_summary_mark_owned_move_requirement(summary, call->arguments[i]);
            }
        }
        xa_summary_walk(summary, call->callee);
        return;
    }

    XaSymbolLinks *fn_links = xa_summary_function_links(summary, call->callee);
    if (fn_links && fn_links->param_effects) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_effect_count; i++) {
            if (xa_param_effect_retains_or_escapes(&fn_links->param_effects[i]))
                xa_summary_mark_expr(summary, call->arguments[i]);
        }
    }
    if (fn_links && fn_links->param_effects) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_effect_count; i++) {
            if (xa_param_effect_mutates(&fn_links->param_effects[i]))
                xa_summary_mark_mutation(summary, call->arguments[i]);
        }
    }
    if (fn_links && fn_links->param_effects) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_effect_count; i++) {
            if (fn_links->param_effects[i].storage_domain == XR_STORAGE_TRANSFERABLE)
                xa_summary_mark_owned_move_requirement(summary, call->arguments[i]);
        }
    }
    if (!fn_links)
        xa_summary_mark_unknown_function_value_args(summary, call);
    xa_summary_walk(summary, call->callee);
    for (int i = 0; i < call->arg_count; i++)
        xa_summary_walk(summary, call->arguments[i]);
}

static void xa_summary_walk_function_expr(XaParamEscapeSummary *summary, AstNode *node) {
    FunctionDeclNode *fn = &node->as.function_expr;
    if (!fn->body)
        return;
    int saved_alias_count = summary->alias_count;
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        xa_summary_set_alias(summary, param ? param->name : NULL, -1);
    }
    xa_summary_mark_capture_refs(summary, fn->body);
    xa_summary_walk(summary, fn->body);
    summary->alias_count = saved_alias_count;
}

static bool xa_summary_return_type_escapes_borrowed_value(XrType *return_type) {
    return !return_type || XR_TYPE_IS_UNKNOWN(return_type) ||
           (return_type && XR_TYPE_IS_POINTER(return_type)) ||
           xa_type_needs_borrow_escape_guard(return_type);
}

void xa_bind_declared_view_origins(XaInferContext *ctx, AstNode *node, XaSymbolLinks *links,
                                   const char **param_names, bool has_receiver) {
    if (!ctx || !ctx->analyzer || !node || !links || !links->type ||
        !XR_TYPE_IS_FUNCTION(links->type))
        return;
    XrBorrowOriginSyntaxState syntax = XR_BORROW_ORIGIN_OMITTED;
    const AstBorrowOriginRef *origins = NULL;
    int origin_count = 0;
    const char *kind = "function";
    const char *name = "?";
    if (node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *decl = &node->as.function_decl;
        syntax = decl->borrow_origin_syntax;
        origins = decl->borrow_origins;
        origin_count = decl->borrow_origin_count;
        name = decl->name ? decl->name : "?";
    } else if (node->type == AST_METHOD_DECL) {
        MethodDeclNode *decl = &node->as.method_decl;
        syntax = decl->borrow_origin_syntax;
        origins = decl->borrow_origins;
        origin_count = decl->borrow_origin_count;
        kind = "method";
        name = decl->name ? decl->name : "?";
    } else if (node->type == AST_INTERFACE_METHOD) {
        InterfaceMethodNode *decl = &node->as.interface_method;
        syntax = decl->borrow_origin_syntax;
        origins = decl->borrow_origins;
        origin_count = decl->borrow_origin_count;
        kind = "interface method";
        name = decl->name ? decl->name : "?";
    } else {
        return;
    }

    XrViewOriginBindStatus status =
        xr_type_function_bind_view_origins(ctx->analyzer->isolate, links->type, param_names, syntax,
                                           origins, origin_count, has_receiver);
    links->return_view.checked = true;
    links->return_view.valid = status == XR_VIEW_ORIGIN_BIND_OK;
    if (status == XR_VIEW_ORIGIN_BIND_OK) {
        int count = links->type->function.view_origin_count;
        if (count > 0) {
            links->return_view.origins =
                (XrViewOrigin *) xr_malloc(sizeof(XrViewOrigin) * (size_t) count);
            if (!links->return_view.origins)
                return;
            memcpy(links->return_view.origins, links->type->function.view_origin_set,
                   sizeof(XrViewOrigin) * (size_t) count);
        }
        links->return_view.origin_count = count;
        links->return_view.was_elided = links->type->function.view_origin_was_elided;
        return;
    }

    const char *code = "OWN-E-VIEW-ORIGIN-INVALID";
    const char *reason = "has an invalid borrowed-result origin set";
    if (status == XR_VIEW_ORIGIN_BIND_AMBIGUOUS) {
        code = "OWN-E-VIEW-ORIGIN-AMBIGUOUS";
        reason = "has multiple eligible signature origins; declare an explicit 'from' set";
    } else if (status == XR_VIEW_ORIGIN_BIND_MUTABLE_RETURN) {
        code = "OWN-E-VIEW-MUTABLE";
        reason = "returns a writable Slice; borrowed returns must be 'const Slice<T>'";
    } else if (status == XR_VIEW_ORIGIN_BIND_HIDDEN_RETURN) {
        reason = "contains a borrowed Slice in a non-direct return type";
    } else if (status == XR_VIEW_ORIGIN_BIND_UNKNOWN_NAME) {
        reason = "names an origin that is not a formal parameter";
    } else if (status == XR_VIEW_ORIGIN_BIND_INELIGIBLE) {
        reason = "names an origin that is not an eligible READ input";
    } else if (status == XR_VIEW_ORIGIN_BIND_NOT_VIEW) {
        reason = "declares 'from' for a return type that is not a borrowed Slice";
    } else if (status == XR_VIEW_ORIGIN_BIND_OUT_OF_MEMORY) {
        reason = "could not allocate its borrowed-result origin set";
    }
    char message[384];
    snprintf(message, sizeof(message), "%s: %s '%s' %s", code, kind, name, reason);
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_BORROW_SOURCE,
                               message, &loc);
}

static void xa_summary_walk(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return;
    switch (node->type) {
        case AST_BLOCK: {
            int saved_alias_count = summary->alias_count;
            for (int i = 0; i < node->as.block.count; i++)
                xa_summary_walk(summary, node->as.block.statements[i]);
            summary->alias_count = saved_alias_count;
            break;
        }
        case AST_EXPR_STMT:
            xa_summary_walk(summary, node->as.expr_stmt);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            VarDeclNode *var = &node->as.var_decl;
            int slot = xa_summary_expr_root_param_slot(summary, var->initializer);
            xa_summary_set_alias(summary, var->name, slot);
            xa_summary_walk(summary, var->initializer);
            break;
        }
        case AST_ASSIGNMENT: {
            AssignmentNode *assign = &node->as.assignment;
            int target_slot = xa_summary_declared_param_slot(summary, assign->name);
            if (target_slot >= 0 && target_slot < summary->param_count) {
                summary->effects[target_slot].access |= XA_PARAM_ACCESS_WRITE;
                summary->effects[target_slot].mutation_paths |= XA_MUTATION_PATH_WILDCARD;
            }
            int slot = xa_summary_expr_root_param_slot(summary, assign->value);
            if (target_slot < 0 && xa_summary_name_is_local(summary, assign->name))
                xa_summary_set_alias(summary, assign->name, slot);
            else
                xa_summary_mark_expr(summary, assign->value);
            xa_summary_walk(summary, assign->value);
            break;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            for (int i = 0; i < ret->value_count; i++) {
                if (xa_summary_return_type_escapes_borrowed_value(summary->return_type))
                    xa_summary_mark_return(summary, ret->values[i]);
                xa_summary_walk(summary, ret->values[i]);
            }
            break;
        }
        case AST_MEMBER_SET:
            xa_summary_mark_mutation(summary, node->as.member_set.object);
            xa_summary_walk(summary, node->as.member_set.object);
            xa_summary_mark_expr(summary, node->as.member_set.value);
            xa_summary_walk(summary, node->as.member_set.value);
            break;
        case AST_INDEX_SET:
            xa_summary_mark_mutation(summary, node->as.index_set.array);
            xa_summary_walk(summary, node->as.index_set.array);
            xa_summary_walk(summary, node->as.index_set.index);
            xa_summary_mark_expr(summary, node->as.index_set.value);
            xa_summary_walk(summary, node->as.index_set.value);
            break;
        case AST_COMPOUND_ASSIGNMENT: {
            int slot = xa_summary_declared_param_slot(summary, node->as.compound_assignment.name);
            if (!node->as.compound_assignment.object && slot >= 0 && slot < summary->param_count) {
                summary->effects[slot].access |= XA_PARAM_ACCESS_WRITE;
                summary->effects[slot].mutation_paths |= XA_MUTATION_PATH_WILDCARD;
            }
            xa_summary_mark_mutation(summary, node->as.compound_assignment.object);
            xa_summary_walk(summary, node->as.compound_assignment.object);
            xa_summary_walk(summary, node->as.compound_assignment.value);
            break;
        }
        case AST_INC: {
            int slot = xa_summary_declared_param_slot(summary, node->as.inc.name);
            if (slot >= 0 && slot < summary->param_count) {
                summary->effects[slot].access |= XA_PARAM_ACCESS_WRITE;
                summary->effects[slot].mutation_paths |= XA_MUTATION_PATH_WILDCARD;
            }
            break;
        }
        case AST_DEC: {
            int slot = xa_summary_declared_param_slot(summary, node->as.dec.name);
            if (slot >= 0 && slot < summary->param_count) {
                summary->effects[slot].access |= XA_PARAM_ACCESS_WRITE;
                summary->effects[slot].mutation_paths |= XA_MUTATION_PATH_WILDCARD;
            }
            break;
        }
        case AST_CALL_EXPR:
            xa_summary_walk_call(summary, node);
            break;
        case AST_GO_EXPR: {
            AstNode *spawned = node->as.go_expr.expr;
            if (spawned && spawned->type == AST_CALL_EXPR) {
                CallExprNode *call = &spawned->as.call_expr;
                for (int i = 0; i < call->arg_count; i++)
                    xa_summary_mark_owned_move_requirement(summary, call->arguments[i]);
            }
            xa_summary_walk(summary, spawned);
            break;
        }
        case AST_FUNCTION_EXPR:
            xa_summary_walk_function_expr(summary, node);
            break;
        case AST_IF_STMT:
            xa_summary_walk(summary, node->as.if_stmt.condition);
            xa_summary_walk(summary, node->as.if_stmt.then_branch);
            xa_summary_walk(summary, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            xa_summary_walk(summary, node->as.while_stmt.condition);
            xa_summary_walk(summary, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            xa_summary_walk(summary, node->as.for_stmt.initializer);
            xa_summary_walk(summary, node->as.for_stmt.condition);
            xa_summary_walk(summary, node->as.for_stmt.increment);
            xa_summary_walk(summary, node->as.for_stmt.body);
            break;
        case AST_FOR_IN_STMT:
            xa_summary_walk(summary, node->as.for_in_stmt.collection);
            xa_summary_walk(summary, node->as.for_in_stmt.body);
            break;
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                xa_summary_walk(summary, node->as.array_literal.repeat_value);
                xa_summary_walk(summary, node->as.array_literal.repeat_count);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    xa_summary_walk(summary, node->as.array_literal.elements[i]);
            }
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++)
                xa_summary_walk(summary, node->as.object_literal.values[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                xa_summary_walk(summary, node->as.map_literal.keys[i]);
                xa_summary_walk(summary, node->as.map_literal.values[i]);
            }
            break;
        case AST_GROUPING:
            xa_summary_walk(summary, node->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_summary_walk(summary, node->as.unary.operand);
            break;
        case AST_MOVE_EXPR:
            xa_summary_walk(summary, node->as.move_expr.expr);
            break;
        case AST_SLICE_EXPR:
            xa_summary_walk(summary, node->as.slice_expr.source);
            xa_summary_walk(summary, node->as.slice_expr.start);
            xa_summary_walk(summary, node->as.slice_expr.end);
            break;
        case AST_MEMBER_ACCESS:
            xa_summary_walk(summary, node->as.member_access.object);
            break;
        case AST_INDEX_GET:
            xa_summary_walk(summary, node->as.index_get.array);
            xa_summary_walk(summary, node->as.index_get.index);
            break;
        case AST_TERNARY:
            xa_summary_walk(summary, node->as.ternary.condition);
            xa_summary_walk(summary, node->as.ternary.true_expr);
            xa_summary_walk(summary, node->as.ternary.false_expr);
            break;
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
        case AST_NULLISH_COALESCE:
            xa_summary_walk(summary, node->as.binary.left);
            xa_summary_walk(summary, node->as.binary.right);
            break;
        default:
            break;
    }
}

static bool xa_symbol_links_set_param_escape_summary(XaInferContext *ctx, XaSymbolLinks *links,
                                                     XrType **param_types, const char **param_names,
                                                     int param_count, XrType *return_type,
                                                     AstNode *body, XrClassInfo *receiver_info) {
    if (!links || param_count < 0 || !body)
        return false;
    XaParamEffectSummary *effects =
        param_count > 0 ? xr_calloc((size_t) param_count, sizeof(XaParamEffectSummary)) : NULL;
    if (param_count > 0 && !effects)
        return false;
    for (int i = 0; i < param_count; i++) {
        XrParamMode mode = xr_type_function_param_mode(links->type, i);
        effects[i].formal_mode = mode;
        effects[i].capability = mode == XR_PARAM_MOVE  ? XA_CAPABILITY_UNIQUE_OWNER
                                : mode == XR_PARAM_REF ? XA_CAPABILITY_EXCLUSIVE_WRITE
                                                       : XA_CAPABILITY_READONLY;
        effects[i].access = XA_PARAM_ACCESS_READ;
        effects[i].callable_effects = links->effect_id;
        effects[i].memory_effects = links->memory_effect_id;
        effects[i].complete = true;
    }
    XaParamEscapeSummary summary = {
        .param_types = param_types,
        .param_names = param_names,
        .param_count = param_count,
        .effects = effects,
        .return_type = return_type,
        .ctx = ctx,
        .receiver_info = receiver_info,
        .declared_view_origins = links->type && XR_TYPE_IS_FUNCTION(links->type)
                                     ? links->type->function.view_origin_set
                                     : NULL,
        .declared_view_origin_count = links->type && XR_TYPE_IS_FUNCTION(links->type)
                                          ? links->type->function.view_origin_count
                                          : 0};
    xa_summary_walk(&summary, body);

    bool changed =
        links->param_effect_count != param_count ||
        (param_count > 0 && (!links->param_effects ||
                             memcmp(links->param_effects, effects,
                                    (size_t) param_count * sizeof(XaParamEffectSummary)) != 0));
    if (links->param_effects)
        xr_free(links->param_effects);
    links->param_effects = effects;
    links->param_effect_count = param_count;
    return changed;
}

void xa_validate_declared_view_origin_returns(XaInferContext *ctx, XaSymbolLinks *links,
                                              AstNode *body, XrClassInfo *receiver_info) {
    if (!ctx || !links || !body || !links->return_view.checked || !links->return_view.valid ||
        links->return_view.origin_count <= 0)
        return;
    int param_count = links->param_count;
    XaParamEffectSummary *effects =
        param_count > 0 ? xr_calloc((size_t) param_count, sizeof(*effects)) : NULL;
    if (param_count > 0 && !effects)
        return;
    XaParamEscapeSummary summary = {
        .param_types = links->param_types,
        .param_names = links->param_names,
        .param_count = param_count,
        .effects = effects,
        .return_type = links->return_type,
        .ctx = ctx,
        .receiver_info = receiver_info,
        .declared_view_origins = links->return_view.origins,
        .declared_view_origin_count = links->return_view.origin_count,
        .validate_view_origins = true,
    };
    xa_summary_walk(&summary, body);
    xr_free(effects);
}

bool xa_function_expr_param_mutates(XaInferContext *ctx, XrType *function_type,
                                    XrParamNode **params, int param_count, AstNode *body,
                                    int param_slot, bool *out_complete) {
    if (out_complete)
        *out_complete = false;
    if (!ctx || !function_type || !XR_TYPE_IS_FUNCTION(function_type) || !body || !params ||
        param_count <= 0 || function_type->function.param_count != param_count ||
        !function_type->function.params || param_slot < 0 || param_slot >= param_count)
        return false;

    const char *stack_names[16];
    XrType *stack_types[16];
    const char **names =
        param_count <= 16 ? stack_names : xr_malloc(sizeof(const char *) * (size_t) param_count);
    XrType **types =
        param_count <= 16 ? stack_types : xr_malloc(sizeof(XrType *) * (size_t) param_count);
    if (!names || !types) {
        if (names && names != stack_names)
            xr_free((void *) names);
        if (types && types != stack_types)
            xr_free(types);
        return false;
    }
    for (int i = 0; i < param_count; i++) {
        names[i] = params[i] ? params[i]->name : NULL;
        types[i] = function_type->function.params[i].type;
    }

    XaSymbolLinks temporary = {.type = function_type};
    xa_symbol_links_set_param_escape_summary(ctx, &temporary, types, names, param_count,
                                             function_type->function.return_type, body, NULL);
    const XaParamEffectSummary *effect = xa_symbol_param_effect(&temporary, param_slot);
    bool mutates = xa_param_effect_mutates(effect);
    if (out_complete)
        *out_complete = effect && effect->complete;

    if (temporary.param_effects)
        xr_free(temporary.param_effects);
    if (names != stack_names)
        xr_free((void *) names);
    if (types != stack_types)
        xr_free(types);
    return mutates;
}

void xa_apply_param_storage_requirements_to_scope(XaInferContext *ctx, XaSymbolLinks *links) {
    if (!ctx || !ctx->analyzer || !links || !links->param_effects || !links->param_names)
        return;
    int count = links->param_count < links->param_effect_count ? links->param_count
                                                               : links->param_effect_count;
    for (int i = 0; i < count; i++) {
        if (links->param_effects[i].storage_domain != XR_STORAGE_TRANSFERABLE ||
            !links->param_names[i])
            continue;
        XaSymbol *param =
            xa_scope_lookup_local(ctx->analyzer->current_scope, links->param_names[i]);
        if (!param || param->kind != XA_SYM_PARAMETER)
            continue;
        param->is_rebindable = false;
    }
}

static XrClassInfo *xa_type_class_info(XrType *type) {
    if (!type || (!XR_TYPE_IS_INSTANCE(type) && !XR_TYPE_IS_CLASS(type)))
        return NULL;
    return type->instance.class_ref;
}

static bool xa_propagate_function_param_escape_summary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_FUNCTION_DECL)
        return false;
    FunctionDeclNode *fn = &node->as.function_decl;
    if (!fn->name)
        return false;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, fn->name);
    if (!sym && ctx->analyzer->global_scope)
        sym = xa_scope_lookup(ctx->analyzer->global_scope, fn->name);
    if (!sym || sym->kind != XA_SYM_FUNCTION)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return xa_symbol_links_set_param_escape_summary(
        ctx, links, links ? links->param_types : NULL, links ? links->param_names : NULL,
        links ? links->param_count : 0, links ? links->return_type : NULL, fn->body, NULL);
}

static bool xa_propagate_class_param_escape_summaries(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node || (node->type != AST_CLASS_DECL && node->type != AST_STRUCT_DECL))
        return false;
    ClassDeclNode *cls =
        (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl : &node->as.class_decl;
    if (!cls->name)
        return false;

    XaSymbol *class_sym =
        cls->symbol_id ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, cls->symbol_id) : NULL;
    if (!class_sym)
        class_sym = xa_scope_lookup(ctx->analyzer->current_scope, cls->name);
    if (!class_sym)
        class_sym = xa_scope_lookup(ctx->analyzer->global_scope, cls->name);
    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
    XrClassInfo *info = class_links ? class_links->class_info : NULL;
    if (!info)
        return false;

    bool changed = false;
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods ? cls->methods[i] : NULL;
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
        XaSymbolLinks *method_links =
            method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
        if (!method_links)
            continue;
        if (xa_symbol_links_set_param_escape_summary(
                ctx, method_links, method_links->param_types, method_links->param_names,
                method_links->param_count, method_links->return_type, md->body, info)) {
            changed = true;
        }
    }
    xa_analyzer_exit_scope(ctx->analyzer);
    return changed;
}

XR_FUNC bool xa_propagate_param_escape_summaries_for_ast(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return false;

    bool changed = false;
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                if (xa_propagate_param_escape_summaries_for_ast(ctx,
                                                                node->as.program.statements[i]))
                    changed = true;
            }
            return changed;
        case AST_EXPORT_STMT:
            return false;
        case AST_FUNCTION_DECL:
            return xa_propagate_function_param_escape_summary(ctx, node);
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            return xa_propagate_class_param_escape_summaries(ctx, node);
        default:
            return false;
    }
}

static uint32_t xa_class_decl_derive_flags(XrAttribute **attrs, int count) {
    uint32_t flags = 0;
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == ATTR_DERIVE)
            flags |= attrs[i]->derive_flags;
    }
    return flags;
}

// Recursively collect all return types from a statement tree
static void collect_return_types(XaInferContext *ctx, AstNode *node, XrType ***types, int *count,
                                 int *cap) {
    XR_DCHECK(ctx != NULL, "collect_return_types: NULL ctx");
    if (!node)
        return;

    switch (node->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            XrType *rt = xr_type_new_unit(NULL);
            if (ret->value_count == 1 && ret->values && ret->values[0]) {
                rt = xa_visit_infer(ctx, ret->values[0]);
            } else if (ret->value_count > 1) {
                XrType **elems = xr_malloc(sizeof(XrType *) * ret->value_count);
                for (int i = 0; i < ret->value_count; i++) {
                    elems[i] = ret->values[i] ? xa_visit_infer(ctx, ret->values[i])
                                              : xr_type_new_unknown(NULL);
                }
                rt = xr_type_new_tuple(ctx->analyzer->isolate, elems, ret->value_count);
                xr_free(elems);
            }
            // Add to collected types
            if (*count >= *cap) {
                int new_cap = *cap ? *cap * 2 : 8;
                XrType **tmp = xr_realloc(*types, sizeof(XrType *) * new_cap);
                if (!tmp)
                    break;
                *types = tmp;
                *cap = new_cap;
            }
            (*types)[(*count)++] = rt;
            break;
        }
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            for (int i = 0; i < block->count; i++) {
                collect_return_types(ctx, block->statements[i], types, count, cap);
            }
            break;
        }
        case AST_IF_STMT:
            collect_return_types(ctx, node->as.if_stmt.then_branch, types, count, cap);
            collect_return_types(ctx, node->as.if_stmt.else_branch, types, count, cap);
            break;
        case AST_WHILE_STMT:
            collect_return_types(ctx, node->as.while_stmt.body, types, count, cap);
            break;
        case AST_FOR_STMT:
            collect_return_types(ctx, node->as.for_stmt.body, types, count, cap);
            break;
        case AST_FOR_IN_STMT:
            collect_return_types(ctx, node->as.for_in_stmt.body, types, count, cap);
            break;
        case AST_TRY_CATCH:
            collect_return_types(ctx, node->as.try_catch.try_body, types, count, cap);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    collect_return_types(ctx, cc->body, types, count, cap);
            }
            // finally return is NOT collected: a return inside finally overrides the
            // try/catch return value entirely, so it must not be unioned with them.
            break;
        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            for (int i = 0; i < m->arm_count; i++) {
                if (m->arms[i] && m->arms[i]->type == AST_MATCH_ARM) {
                    collect_return_types(ctx, m->arms[i]->as.match_arm.body, types, count, cap);
                }
            }
            break;
        }
        default:
            break;
    }
}

// Infer return type by scanning all return statements in function/method body
XrType *xa_infer_function_return_type(XaInferContext *ctx, AstNode *body) {
    if (!body)
        return NULL;

    XrType **types = NULL;
    int count = 0, cap = 0;
    collect_return_types(ctx, body, &types, &count, &cap);

    if (count == 0) {
        if (types)
            xr_free(types);
        return NULL;
    }

    // Union all collected return types
    XrType *result = types[0];
    for (int i = 1; i < count; i++) {
        if (!xr_type_equals(result, types[i])) {
            result = xr_type_union(ctx->analyzer->isolate, result, types[i]);
        }
    }

    xr_free(types);
    return result;
}

static XrClassInfo *xa_default_init_class_info(XaInferContext *ctx, XrType *type) {
    if (!ctx || !ctx->analyzer || !type)
        return NULL;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_ref) {
        return type->instance.class_ref;
    }
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;

    const char *name = type->instance.class_name;
    if (!name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(ctx->analyzer, name);
    if (!sym || sym->kind != XA_SYM_CLASS) {
        sym = xa_analyzer_lookup_in_scope(ctx->analyzer, name, ctx->analyzer->global_scope);
    }
    if (!sym || sym->kind != XA_SYM_CLASS) {
        sym = xa_analyzer_lookup_deep(ctx->analyzer, name);
    }
    if (!sym || sym->kind != XA_SYM_CLASS)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return links ? links->class_info : NULL;
}

static bool xa_type_is_default_initializable_depth(XaInferContext *ctx, XrType *type, int depth) {
    if (!type)
        return false;
    if (depth > 16)
        return false;

    if (xr_type_is_default_initializable(type))
        return true;

    if (type->kind == XR_KIND_FIXED_ARRAY) {
        return type->fixed_array.length >= 0 && xa_type_is_default_initializable_depth(
                                                    ctx, type->fixed_array.element_type, depth + 1);
    }

    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return false;

    XrClassInfo *info = xa_default_init_class_info(ctx, type);
    if (!info)
        return false;

    bool is_struct = type->is_value_type || info->struct_layout != NULL;
    if (!is_struct)
        return false;

    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *field = info->fields[i];
        XaSymbolLinks *links = field ? xa_analyzer_get_links(ctx->analyzer, field) : NULL;
        if (!links || !links->type)
            return false;
        if (!xa_type_is_default_initializable_depth(ctx, links->type, depth + 1))
            return false;
    }
    return true;
}

bool xa_type_is_default_initializable(XaInferContext *ctx, XrType *type) {
    return xa_type_is_default_initializable_depth(ctx, type, 0);
}

/* True when a struct field is a generic value-struct instance (e.g. `Box<int>`)
 * whose concrete monomorphized instance has not been registered yet. This
 * happens during the first analysis pass, before the mono pass materializes
 * `Box$i64`. The caller defers the aggregate layout to the post-mono
 * re-analysis (see xa_visit_collect_class) instead of rejecting the field. */
static bool xa_field_is_pending_generic_value_struct(XaInferContext *ctx, const XrType *ft,
                                                     const char *field_class_name) {
    if (!ctx || !ctx->analyzer || !ft || !field_class_name)
        return false;
    if (ft->kind != XR_KIND_INSTANCE || ft->instance.type_arg_count <= 0)
        return false;
    XaSymbol *head_sym = xa_analyzer_lookup(ctx->analyzer, field_class_name);
    XaSymbolLinks *head_links = head_sym ? xa_analyzer_get_links(ctx->analyzer, head_sym) : NULL;
    return head_links && head_links->type && head_links->type->is_value_type;
}

// Phase 1: Collect function declaration only (symbol, not body).
// Cross-TU: called from xa_visit_collect_statements_with_hoisting() in
// xanalyzer_visitor.c during the hoisting pass.
void xa_visit_collect_function_decl_only(XaInferContext *ctx, AstNode *node) {
    if (!node)
        return;

    FunctionDeclNode *fn = &node->as.function_decl;

    // Create the function symbol, or reuse it on the post-monomorphization
    // re-analysis. Reusing keeps the symbol id stable so already-resolved call
    // sites (which cache callee->symbol_id) observe the refreshed signature.
    // This matters for generic value-struct parameters and return types: the
    // first analysis resolves `Box<int>` to a generic instance, and the second
    // analysis (after the mono pass materializes `Box$i64`) re-resolves it to
    // the concrete monomorphized instance. Without a stable symbol the caller
    // would keep reading the stale first-pass signature and reject the argument
    // or mistype the result.
    XaSymbol *sym = NULL;
    if (fn->symbol_id != 0)
        sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, fn->symbol_id);
    /* A second declaration under an already-bound name is rejected wholesale:
     * two bodies feeding one name make the error-set fixpoint oscillate and
     * never converge, so the duplicate must not register a symbol, links, or
     * an analyzable body at all. The re-analysis path (symbol_id already
     * bound to this declaration) is exempt — that is the same declaration. */
    if (!sym && fn->name && ctx->analyzer && ctx->analyzer->current_scope) {
        XaSymbol *bound = xa_scope_lookup_local(ctx->analyzer->current_scope, fn->name);
        if (bound && bound->kind == XA_SYM_FUNCTION && (int) bound->location.line != node->line) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Symbol '%s' is redefined in the same scope", fn->name);
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_CMP_REDEFINED_VAR,
                                       msg, &loc);
            return;
        }
    }
    if (!sym || sym->kind != XA_SYM_FUNCTION)
        sym = xa_symbol_new(fn->name, XA_SYM_FUNCTION);
    sym->location.line = node->line;
    sym->is_const = true;
    sym->is_exported = node->is_exported;
    XaScope *signature_scope = ctx->analyzer ? ctx->analyzer->current_scope : NULL;
    XaSymbol *saved_signature_function = signature_scope ? signature_scope->function_symbol : NULL;
    if (fn->type_param_count > 0 && fn->type_params) {
        const char **type_param_names = xr_malloc(sizeof(const char *) * fn->type_param_count);
        if (type_param_names) {
            for (int i = 0; i < fn->type_param_count; i++)
                type_param_names[i] = fn->type_params[i] ? fn->type_params[i]->name : NULL;
            xa_symbol_links_set_type_params(&sym->links, type_param_names, NULL, NULL,
                                            fn->type_param_count);
            xr_free(type_param_names);
        }
        if (signature_scope)
            signature_scope->function_symbol = sym;
    }

    // Build function type and collect param names
    XrType **param_types = NULL;
    const char **param_names = NULL;
    bool has_rest = false;

    if (fn->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType *) * fn->param_count);
        param_names = xr_malloc(sizeof(const char *) * fn->param_count);
        if (!param_types || !param_names) {
            xr_free(param_types);
            xr_free(param_names);
            if (signature_scope)
                signature_scope->function_symbol = saved_signature_function;
            return;
        }
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *param = fn->params[i];
            param_types[i] = (param && param->type)
                                 ? xr_tref_resolve_parameter_in_analyzer(ctx->analyzer, param->type)
                                 : xr_type_new_unknown(NULL);
            param_names[i] = param ? param->name : NULL;
            if (param && param->is_rest)
                has_rest = true;

            // Warn: function parameter missing type annotation
            if (param && !param->type && !param->is_rest) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Parameter '%s' of function '%s' is missing type annotation", param->name,
                         fn->name ? fn->name : "<anonymous>");
                XrLocation loc = {
                    .file = ctx->file_path, .line = param->line, .column = param->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
            }
        }
    }

    // Omitted return type defaults to void; error if body has 'return <expr>'
    XrType *return_type = fn->return_type
                              ? xr_tref_resolve_in_analyzer(ctx->analyzer, fn->return_type)
                              : xr_type_new_unit(NULL);
    if (!fn->return_type && fn->name && fn->body) {
        if (xa_body_has_return_expr(fn->body)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Function '%s' returns a value but has no return type annotation", fn->name);
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
        }
    }
    // Resolve CLASS("T") → TYPE_PARAM("T") for generic functions
    if (fn->type_param_count > 0 && fn->type_params) {
        const char *tp_buf[8];
        const char **tp_names = (fn->type_param_count <= 8)
                                    ? tp_buf
                                    : xr_malloc(sizeof(const char *) * fn->type_param_count);
        for (int i = 0; i < fn->type_param_count; i++)
            tp_names[i] = fn->type_params[i]->name;
        for (int i = 0; i < fn->param_count; i++)
            param_types[i] =
                resolve_class_to_type_param(NULL, param_types[i], tp_names, fn->type_param_count);
        return_type =
            resolve_class_to_type_param(NULL, return_type, tp_names, fn->type_param_count);
        if (tp_names != tp_buf)
            xr_free((void *) tp_names);
    }
    if (signature_scope)
        signature_scope->function_symbol = saved_signature_function;

    for (int i = 0; i < fn->param_count; i++) {
        char context[160];
        snprintf(context, sizeof(context), "function parameter '%s'",
                 param_names && param_names[i] ? param_names[i] : "?");
        xa_freestanding_report_tagged_type_unavailable(ctx, node, param_types[i], context);
    }
    xa_freestanding_report_tagged_type_unavailable(ctx, node, return_type, "function return type");

    XrType *fn_type = xr_type_new_function(ctx->analyzer->isolate, param_types, fn->param_count,
                                           return_type, has_rest);
    if (fn_type && fn->body)
        xr_type_function_set_throw_effect(fn_type, XR_FN_EFFECT_POLY);
    xa_set_function_type_params_from_ast(ctx, fn_type, fn->type_params, fn->type_param_count);

    // Set min_params for default parameter support
    if (fn_type) {
        fn_type->function.min_params = fn->required_count;

        for (int i = 0; i < fn->param_count; i++) {
            if (fn->params[i])
                xr_type_function_set_param_mode(fn_type, i, fn->params[i]->passing_mode);
        }
    }

    // Add to scope
    xa_visit_add_symbol_checked(ctx, sym, 0);
    fn->symbol_id = sym->id;

    // Create symbol links with type and param names
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->type = fn_type;
    links->declared_type = fn_type;
    links->binding_use = XA_BINDING_LIVE;
    links->binding_mutability = XA_BINDING_STABLE;
    links->value_capability = XA_CAP_CONST;
    links->storage_domain =
        ctx->analyzer->current_scope && ctx->analyzer->current_scope->kind == XA_SCOPE_GLOBAL
            ? XR_STORAGE_MODULE_STATIC
            : XR_STORAGE_EXEC_LOCAL;
    links->file_path = ctx->file_path;
    links->function_decl_node = node;
    xa_publish_deprecated_attrs(links, fn->attributes, fn->attr_count);
    xa_bind_registry_intrinsic(ctx, node, sym, NULL, fn->name, false, fn->param_count);

    // FFI: mark extern-block functions so call sites can require `unsafe { }`.
    links->is_extern = fn->is_extern;
    if (links->is_extern)
        xa_validate_extern_function_abi(ctx, node, fn, param_types, return_type);
    if (links->is_extern) {
        const XrNativePackagePlan *native_plan = xr_compiler_session_native_package_plan(
            ctx->analyzer ? ctx->analyzer->compiler_session : NULL);
        if (native_plan) {
            char native_error[512];
            XrLocation native_loc = {.file = ctx->file_path,
                                     .line = node ? node->line : 0,
                                     .column = node ? node->column : 0};
            if (!xr_native_package_validate_symbol_arity(native_plan, fn->name,
                                                         (uint32_t) fn->param_count, native_error,
                                                         sizeof(native_error)))
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_ARG_TYPE, native_error, &native_loc);
        }
    }
    // Store parameter names for LSP inlay hints
    xa_symbol_links_set_function_sig(links, param_types, param_names, fn->param_count, return_type);
    xa_bind_declared_view_origins(ctx, node, links, param_names, false);
    xa_symbol_links_set_param_escape_summary(ctx, links, param_types, param_names, fn->param_count,
                                             return_type, fn->body, NULL);

    // Record per-parameter default expressions for caller-side default filling.
    if (fn->param_count > 0) {
        AstNode **defs = (AstNode **) xr_calloc(fn->param_count, sizeof(AstNode *));
        if (defs) {
            for (int i = 0; i < fn->param_count; i++)
                defs[i] = fn->params[i] ? fn->params[i]->default_value : NULL;
            xa_bind_param_default_exprs(ctx, defs, param_types, fn->param_count);
            xa_symbol_links_set_param_defaults(links, defs, fn->param_count);
            xr_free(defs);
        }
    }

    // Store generic type parameters and intersection-style constraint lists.
    xa_store_type_params_with_constraints(ctx, links, fn->type_params, fn->type_param_count, node);

    XrLocation sig_loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        XrLocation param_loc = {.file = ctx->file_path,
                                .line = param ? param->line : node->line,
                                .column = param ? param->column : node->column};
        xa_validate_hashable_key_type(ctx, param_types ? param_types[i] : NULL, links,
                                      "function parameter type", &param_loc);
    }
    xa_validate_hashable_key_type(ctx, return_type, links, "function return type", &sig_loc);

    if (param_types)
        xr_free(param_types);
    if (param_names)
        xr_free(param_names);
}

// Collect return-value AST nodes from a function body.
// Only collects object-literal returns; sets out_bad if a non-object, non-null return is found.
static void xa_collect_returns(AstNode *node, AstNode **out, int *count, int cap, bool *out_bad) {
    if (!node || *out_bad)
        return;
    switch (node->type) {
        case AST_BLOCK: {
            BlockNode *blk = &node->as.block;
            for (int i = 0; i < blk->count; i++)
                xa_collect_returns(blk->statements[i], out, count, cap, out_bad);
            break;
        }
        case AST_IF_STMT: {
            IfStmtNode *ifn = &node->as.if_stmt;
            xa_collect_returns(ifn->then_branch, out, count, cap, out_bad);
            if (ifn->else_branch)
                xa_collect_returns(ifn->else_branch, out, count, cap, out_bad);
            break;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            if (ret->value_count == 0)
                break;
            AstNode *val = ret->values[0];
            if (val->type == AST_LITERAL_NULL)
                break;
            if (val->type == AST_OBJECT_LITERAL) {
                if (*count < cap)
                    out[(*count)++] = val;
            } else {
                *out_bad = true;
            }
            break;
        }
        default:
            break;
    }
}

// Infer the structural-object return type when all returns are same-shape object literals.
static XrType *xa_infer_return_object_type(XrVMRuntime *X, FunctionDeclNode *fn) {
    if (!fn->body || fn->return_type)
        return NULL;

    static const int MAX_RETURNS = 32;
    static const int MAX_FIELDS = 32;
    AstNode *rets[32];
    int nrets = 0;
    bool bad = false;
    xa_collect_returns(fn->body, rets, &nrets, MAX_RETURNS, &bad);
    if (bad || nrets == 0)
        return NULL;

    ObjectLiteralNode *first = &rets[0]->as.object_literal;
    int fc = 0;
    for (int i = 0; i < first->count; i++) {
        if (first->keys[i]->type == AST_LITERAL_STRING)
            fc++;
    }
    if (fc == 0 || fc > MAX_FIELDS)
        return NULL;

    // Verify all returns have same static field names (order-insensitive)
    for (int r = 1; r < nrets; r++) {
        ObjectLiteralNode *o = &rets[r]->as.object_literal;
        int ofc = 0;
        for (int i = 0; i < o->count; i++)
            if (o->keys[i]->type == AST_LITERAL_STRING)
                ofc++;
        if (ofc != fc)
            return NULL;
        for (int i = 0; i < first->count; i++) {
            if (first->keys[i]->type != AST_LITERAL_STRING)
                continue;
            const char *fname = first->keys[i]->as.literal.raw_value.string_val;
            bool found = false;
            for (int j = 0; j < o->count; j++) {
                if (o->keys[j]->type != AST_LITERAL_STRING)
                    continue;
                if (strcmp(o->keys[j]->as.literal.raw_value.string_val, fname) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return NULL;
        }
    }

    // Build field name + type arrays from first return's object literal
    const char *names[32];
    XrType *types[32];
    int idx = 0;
    for (int i = 0; i < first->count && idx < 32; i++) {
        if (first->keys[i]->type != AST_LITERAL_STRING)
            continue;
        names[idx] = first->keys[i]->as.literal.raw_value.string_val;
        // Infer field type from AST literal (Pass 1: no full inference available)
        AstNode *val = first->values[i];
        switch (val ? val->type : 0) {
            case AST_LITERAL_INT:
                types[idx] = xr_type_new_int(NULL);
                break;
            case AST_LITERAL_FLOAT:
                types[idx] = xr_type_new_float(NULL);
                break;
            case AST_LITERAL_STRING:
                types[idx] = xr_type_new_string(NULL);
                break;
            case AST_FIXED_BYTES_LITERAL: {
                size_t length = val->as.fixed_bytes_literal.payload_length +
                                (val->as.fixed_bytes_literal.append_nul ? 1u : 0u);
                if (length > INT_MAX)
                    return NULL;
                XrType *byte_type = xr_type_new_int_width(X, XR_NATIVE_U8);
                types[idx] = xr_type_new_fixed_array(X, byte_type, (int) length);
                break;
            }
            case AST_LITERAL_TRUE:
            case AST_LITERAL_FALSE:
                types[idx] = xr_type_new_bool(NULL);
                break;
            case AST_LITERAL_NULL:
                types[idx] = xr_type_new_unknown(NULL);
                break;
            default:
                types[idx] = xr_type_new_unknown(NULL);
                break;
        }
        idx++;
    }
    return xr_type_new_struct_object_with_fields(X, names, types, fc);
}

// Phase 2: Collect function body (parameters and body declarations).
// Cross-TU: called from xa_visit_collect_statements_with_hoisting() in
// xanalyzer_visitor.c after Phase 1 has hoisted all symbols.
void xa_visit_collect_function_body(XaInferContext *ctx, AstNode *node) {
    if (!node)
        return;

    FunctionDeclNode *fn = &node->as.function_decl;

    // Get function type from already-created symbol
    XaSymbol *sym = xa_scope_lookup_local(ctx->analyzer->current_scope, fn->name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;

    // Enter function scope and collect body
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);
    ctx->analyzer->current_scope->function_symbol = sym;

    // Add parameters to scope
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *p = fn->params[i];
        if (p && p->name) {
            XaSymbol *param = xa_visit_bind_parameter_symbol(ctx, p, node->line);
            if (!param)
                continue;

            XaSymbolLinks *param_links = xa_analyzer_get_links(ctx->analyzer, param);
            if (p->is_rest) {
                // Rest parameter is packed into Array at runtime
                XrType *elem_type = (links && links->param_types && i < links->param_count)
                                        ? links->param_types[i]
                                        : xr_type_new_unknown(NULL);
                param_links->type = xr_type_new_array(ctx->analyzer->isolate, elem_type);
            } else {
                param_links->type = (links && links->param_types && i < links->param_count)
                                        ? links->param_types[i]
                                        : xr_type_new_unknown(NULL);
            }
            param_links->is_definitely_assigned = true;
        }
    }

    // Collect body declarations
    if (fn->body) {
        xa_visit_collect(ctx, fn->body);
    }

    if (links) {
        xa_symbol_links_set_param_escape_summary(ctx, links, links->param_types, links->param_names,
                                                 links->param_count, links->return_type, fn->body,
                                                 NULL);
        xa_validate_declared_view_origin_returns(ctx, links, fn->body, NULL);
    }

    // Infer return type for unannotated functions that always return same-shape objects.
    // This updates the function's return type so that call-site type propagation
    // can see a concrete Json type instead of unknown.
    if (links && !fn->return_type) {
        XrType *inferred_ret = xa_infer_return_object_type(ctx->analyzer->isolate, fn);
        if (inferred_ret) {
            links->return_type = inferred_ret;
            links->return_type_inferred = true;
            // Also update the function type object so xa_visit_call sees it
            if (links->type && XR_TYPE_IS_FUNCTION(links->type)) {
                links->type->function.return_type = inferred_ret;
            }
        }
    }

    /* Publish return ownership before leaving the function scope. The scan
     * resolves returned names through the visible scope, so it has to run here
     * rather than on demand from a phase that no longer has this scope. The
     * return type must already be settled, hence after the inference above. */
    if (links)
        xa_ensure_function_return_ownership_prepass(ctx, links);

    xa_analyzer_exit_scope(ctx->analyzer);
}

// Combined: for direct calls (not through hoisting)
void xa_visit_collect_function(XaInferContext *ctx, AstNode *node) {
    xa_visit_collect_function_decl_only(ctx, node);
    xa_visit_collect_function_body(ctx, node);
}

/* ----------------------------------------------------------------------------
 * Constructor super() Validation
 * -------------------------------------------------------------------------- */

// Check if AST node contains 'this' expression (before super() call)
static bool contains_this_expr(AstNode *node) {
    if (!node)
        return false;

    switch (node->type) {
        case AST_THIS_EXPR:
            return true;
        case AST_CALL_EXPR:
            if (contains_this_expr(node->as.call_expr.callee))
                return true;
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                if (contains_this_expr(node->as.call_expr.arguments[i]))
                    return true;
            }
            break;
        case AST_MEMBER_ACCESS:
            return contains_this_expr(node->as.member_access.object);
        case AST_INDEX_GET:
            return contains_this_expr(node->as.index_get.array) ||
                   contains_this_expr(node->as.index_get.index);
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            return contains_this_expr(node->as.binary.left) ||
                   contains_this_expr(node->as.binary.right);
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            return contains_this_expr(node->as.unary.operand);
        case AST_TERNARY:
            return contains_this_expr(node->as.ternary.condition) ||
                   contains_this_expr(node->as.ternary.true_expr) ||
                   contains_this_expr(node->as.ternary.false_expr);
        case AST_ASSIGNMENT:
            return contains_this_expr(node->as.assignment.value);
        default:
            break;
    }
    return false;
}

// Check if statement contains 'this' expression
static bool stmt_contains_this(AstNode *stmt) {
    if (!stmt)
        return false;

    switch (stmt->type) {
        case AST_EXPR_STMT:
            return contains_this_expr(stmt->as.expr_stmt);
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return contains_this_expr(stmt->as.var_decl.initializer);
        case AST_ASSIGNMENT:
            return contains_this_expr(stmt->as.assignment.value);
        case AST_MEMBER_SET:
            return contains_this_expr(stmt->as.member_set.object) ||
                   contains_this_expr(stmt->as.member_set.value);
        case AST_RETURN_STMT:
            for (int i = 0; i < stmt->as.return_stmt.value_count; i++) {
                if (contains_this_expr(stmt->as.return_stmt.values[i]))
                    return true;
            }
            break;
        default:
            break;
    }
    return false;
}

// Validate constructor super() call rules:
// 1. super() must be first statement (if called)
// 2. Cannot access 'this' before super()
// 3. Must call super() if parent has required parameters
static void validate_constructor_super_call(XaInferContext *ctx, ClassDeclNode *cls,
                                            MethodDeclNode *constructor, AstNode *method_node) {
    if (!constructor || !constructor->body)
        return;

    AstNode *body = constructor->body;
    if (body->type != AST_BLOCK)
        return;

    BlockNode *block = &body->as.block;
    bool has_super_call = false;
    int super_call_index = -1;
    int super_call_line = 0;

    // Find super() call position
    for (int i = 0; i < block->count; i++) {
        AstNode *stmt = block->statements[i];
        if (!stmt)
            continue;

        // Check for super() call (as expression statement)
        if (stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
            AstNode *expr = stmt->as.expr_stmt;
            if (expr->type == AST_SUPER_CALL) {
                has_super_call = true;
                super_call_index = i;
                super_call_line = stmt->line;
                break;
            }
        }
        // Also check direct super call statement
        if (stmt->type == AST_SUPER_CALL) {
            has_super_call = true;
            super_call_index = i;
            super_call_line = stmt->line;
            break;
        }
    }

    // Check 1: If class has a parent, validate super() usage
    if (cls->super_name) {
        // Check 2: super() must be first statement (if called)
        if (has_super_call && super_call_index > 0) {
            XrLocation loc = {.file = ctx->file_path, .line = super_call_line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_FIRST,
                                       "super() must be the first statement in constructor", &loc);
        }

        // Check 3: Cannot access 'this' before super()
        if (has_super_call) {
            for (int i = 0; i < super_call_index; i++) {
                AstNode *stmt = block->statements[i];
                if (stmt_contains_this(stmt)) {
                    XrLocation loc = {.file = ctx->file_path, .line = stmt->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_SUPER_THIS,
                                               "Cannot access 'this' before calling super()", &loc);
                    break;
                }
            }
        }

        // Check 4: Smart super() requirement based on parent constructor
        // - Parent has no constructor → no super() needed
        // - Parent constructor has only optional params → auto-insert at codegen
        // - Parent constructor has required params → must call super(args)
        if (!has_super_call) {
            // Look up parent class info (search outside class scope)
            XaSymbol *parent_sym =
                xa_scope_lookup(ctx->analyzer->current_scope->parent, cls->super_name);
            XrClassInfo *parent_info = NULL;
            if (parent_sym) {
                XaSymbolLinks *parent_links = xa_analyzer_get_links(ctx->analyzer, parent_sym);
                if (parent_links)
                    parent_info = parent_links->class_info;
            }

            if (parent_info && parent_info->has_constructor &&
                parent_info->constructor_required_params > 0) {
                // Parent constructor has required params — must call super()
                XrLocation loc = {.file = ctx->file_path, .line = method_node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Constructor must call super() because '%s' constructor requires %d "
                         "argument(s)",
                         cls->super_name, parent_info->constructor_required_params);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_SUPER_REQUIRED, msg, &loc);
            }
            // else: parent has no constructor or all-optional params → OK
        }
    } else {
        // No parent class - super() should not be called
        if (has_super_call) {
            XrLocation loc = {.file = ctx->file_path, .line = super_call_line};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_INVALID,
                "super() can only be called in a class that extends another class", &loc);
        }
    }
}

static bool expr_assigns_this_field(AstNode *expr, const char *field_name) {
    if (!expr || !field_name)
        return false;
    if (expr->type != AST_MEMBER_SET)
        return false;

    MemberSetNode *set = &expr->as.member_set;
    return set->object && set->object->type == AST_THIS_EXPR && set->member &&
           strcmp(set->member, field_name) == 0;
}

static bool stmt_definitely_assigns_this_field(AstNode *stmt, const char *field_name) {
    if (!stmt || !field_name)
        return false;

    switch (stmt->type) {
        case AST_BLOCK: {
            BlockNode *block = &stmt->as.block;
            for (int i = 0; i < block->count; i++) {
                if (stmt_definitely_assigns_this_field(block->statements[i], field_name))
                    return true;
            }
            return false;
        }
        case AST_EXPR_STMT:
            return expr_assigns_this_field(stmt->as.expr_stmt, field_name);
        case AST_MEMBER_SET:
            return expr_assigns_this_field(stmt, field_name);
        case AST_IF_STMT:
            return stmt->as.if_stmt.then_branch && stmt->as.if_stmt.else_branch &&
                   stmt_definitely_assigns_this_field(stmt->as.if_stmt.then_branch, field_name) &&
                   stmt_definitely_assigns_this_field(stmt->as.if_stmt.else_branch, field_name);
        default:
            return false;
    }
}

static bool class_has_bodyless_constructor(ClassDeclNode *cls) {
    if (!cls)
        return false;
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (md->is_constructor && !md->body)
            return true;
    }
    return false;
}

static bool class_constructors_assign_field(ClassDeclNode *cls, const char *field_name) {
    if (!cls || !field_name)
        return false;

    bool saw_constructor = false;
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (!md->is_constructor)
            continue;
        saw_constructor = true;
        if (!md->body)
            continue;
        if (!stmt_definitely_assigns_this_field(md->body, field_name))
            return false;
    }
    return saw_constructor;
}

static void validate_class_field_default_initialization(XaInferContext *ctx, AstNode *node,
                                                        ClassDeclNode *cls, XrClassInfo *info) {
    if (!ctx || !node || !cls || !info)
        return;
    if (node->type != AST_CLASS_DECL)
        return;
    if (class_has_bodyless_constructor(cls))
        return;

    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field_node = cls->fields[i];
        if (!field_node || field_node->type != AST_FIELD_DECL)
            continue;

        FieldDeclNode *fd = &field_node->as.field_decl;
        if (fd->initializer)
            continue;

        XaSymbol *field_sym = xa_class_info_lookup_member(info, fd->name);
        XaSymbolLinks *links = field_sym ? xa_analyzer_get_links(ctx->analyzer, field_sym) : NULL;
        XrType *field_type = links ? links->type : NULL;
        if (!field_type || XR_TYPE_IS_UNKNOWN(field_type))
            continue;
        if (xa_type_is_default_initializable(ctx, field_type))
            continue;
        if (!fd->is_static && class_constructors_assign_field(cls, fd->name))
            continue;

        XrLocation loc = {.file = ctx->file_path, .line = field_node->line};
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Field '%s' of class '%s' has type '%s' and must have an initializer or be "
                 "assigned in every constructor",
                 fd->name ? fd->name : "?", cls->name ? cls->name : "?",
                 xr_type_to_string(field_type));
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
    }
}

// Register a user-defined interface as a class-shaped symbol so the rest of
// the analyzer (constraint checks, conformance lookups, type-arg resolution)
// can find it through xa_scope_lookup.  Method and property signatures are
// kept on info->methods / info->fields, mirroring what xa_visit_collect_class
// does for real classes — that is what lets the conformance check at the end
// of class collection enforce method-name parity.
void xa_visit_collect_interface(XaInferContext *ctx, AstNode *node) {
    if (!node || node->type != AST_INTERFACE_DECL)
        return;

    InterfaceDeclNode *iface = &node->as.interface_decl;
    if (xa_reject_builtin_name_redeclaration(ctx, node, "interface", iface->name))
        return;

    XaSymbol *sym = xa_symbol_new(iface->name, XA_SYM_CLASS);
    sym->location.line = node->line;
    sym->is_exported = node->is_exported;
    xa_visit_add_symbol_checked(ctx, sym, 0);

    XrClassInfo *info = xa_class_info_new(iface->name);
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->class_info = info;
    links->owns_class_info = true;
    // Represent the interface as a parameterized XR_KIND_INTERFACE: built-in
    // singletons stay as plain interface types; user `interface Foo<T>` keeps
    // its declared type parameters so generic resolution can plug arguments
    // in later (the type_args slot is empty at the declaration site).
    links->type = xr_type_new_interface(ctx->analyzer->isolate, iface->name);
    // Carry the declaration identity on the type, exactly as a class instance
    // does. Interface names are ordinary identifiers too, so `interface
    // Lengthable { ... }` is legal source and must not be mistaken for the
    // builtin of that name: xr_type_is_builtin_named_type reads a NULL
    // class_ref as "builtin", and the builtin interface registry leaves it NULL.
    if (links->type)
        links->type->instance.class_ref = info;
    info->base = NULL;  // interfaces never carry an inheritance chain here
    info->base_name = NULL;

    if (iface->type_param_count > 0 && iface->type_params) {
        const char **type_param_names =
            xr_malloc(sizeof(const char *) * (size_t) iface->type_param_count);
        if (type_param_names) {
            for (int i = 0; i < iface->type_param_count; i++) {
                type_param_names[i] = iface->type_params[i] ? iface->type_params[i]->name : NULL;
            }
            xa_symbol_links_set_type_params(links, type_param_names, NULL, NULL,
                                            iface->type_param_count);
            xr_free(type_param_names);
        }
    }

    // Materialise method and property signatures as XaSymbols. Names matter
    // for conformance; types are best-effort (resolved from XrTypeRef) so the
    // later signature audit can still inspect them when needed.
    for (int i = 0; i < iface->method_count; i++) {
        AstNode *m = iface->methods ? iface->methods[i] : NULL;
        if (!m || m->type != AST_INTERFACE_METHOD)
            continue;
        InterfaceMethodNode *im = &m->as.interface_method;
        if (!im->name)
            continue;
        XaSymbol *msym = xa_symbol_new(im->name, XA_SYM_METHOD);
        msym->location.line = m->line;
        msym->receiver_mode = im->receiver_mode;
        XaSymbolLinks *mlinks = xa_analyzer_get_links(ctx->analyzer, msym);
        xa_publish_deprecated_attrs(mlinks, im->attributes, im->attr_count);
        // An interface method may carry its own generic params and constraints.
        // They are stored, and the method published as the signature scope's
        // active generic owner, before the signature is resolved — that is what
        // lets `x: T` resolve to a type parameter instead of an undefined type,
        // and what lets the constraint-aware checks see the bounds.
        XaScope *signature_scope = ctx->analyzer ? ctx->analyzer->current_scope : NULL;
        XaSymbol *saved_signature_function =
            signature_scope ? signature_scope->function_symbol : NULL;
        if (im->type_param_count > 0 && im->type_params) {
            xa_store_type_params_with_constraints(ctx, mlinks, im->type_params,
                                                  im->type_param_count, m);
            if (signature_scope)
                signature_scope->function_symbol = msym;
        }
        XrType **param_types = NULL;
        if (im->param_count > 0) {
            param_types = xr_malloc(sizeof(XrType *) * im->param_count);
            if (!param_types)
                continue;
            for (int j = 0; j < im->param_count; j++) {
                XrParamNode *param = im->params ? im->params[j] : NULL;
                param_types[j] =
                    (param && param->type)
                        ? xr_tref_resolve_parameter_in_analyzer(ctx->analyzer, param->type)
                        : xr_type_new_unknown(NULL);
            }
        }
        XrType *ret_type = im->return_type
                               ? xr_tref_resolve_in_analyzer(ctx->analyzer, im->return_type)
                               : xr_type_new_unit(NULL);
        // Resolve CLASS("T") → TYPE_PARAM("T") for generic interface methods,
        // so a bound `T` in the signature is a type parameter rather than an
        // undefined class name.
        if (im->type_param_count > 0 && im->type_params) {
            const char **tp_names = xr_malloc(sizeof(const char *) * (size_t) im->type_param_count);
            if (tp_names) {
                for (int j = 0; j < im->type_param_count; j++)
                    tp_names[j] = im->type_params[j] ? im->type_params[j]->name : NULL;
                for (int j = 0; j < im->param_count; j++) {
                    param_types[j] = resolve_class_to_type_param(NULL, param_types[j], tp_names,
                                                                 im->type_param_count);
                }
                ret_type =
                    resolve_class_to_type_param(NULL, ret_type, tp_names, im->type_param_count);
                xr_free(tp_names);
            }
        }
        if (signature_scope)
            signature_scope->function_symbol = saved_signature_function;
        mlinks->type = xr_type_new_function(ctx->analyzer->isolate, param_types, im->param_count,
                                            ret_type, false);
        if (mlinks->type)
            mlinks->type->function.receiver_mode = im->receiver_mode;
        if (mlinks->type && im->params) {
            for (int j = 0; j < im->param_count; j++) {
                XrParamNode *param = im->params[j];
                xr_type_function_set_param_mode(mlinks->type, j,
                                                param ? param->passing_mode : XR_PARAM_READ);
            }
        }
        const char **origin_param_names = NULL;
        if (im->param_count > 0) {
            origin_param_names =
                (const char **) xr_malloc(sizeof(const char *) * (size_t) im->param_count);
            if (origin_param_names) {
                for (int j = 0; j < im->param_count; j++)
                    origin_param_names[j] =
                        im->params && im->params[j] ? im->params[j]->name : NULL;
            }
        }
        xa_symbol_links_set_function_sig(mlinks, param_types, origin_param_names, im->param_count,
                                         ret_type);
        xa_bind_declared_view_origins(ctx, m, mlinks, origin_param_names, true);
        xr_free(origin_param_names);
        if (param_types)
            xr_free(param_types);
        xa_class_info_add_method(info, msym);
    }

    for (int i = 0; i < iface->property_count; i++) {
        AstNode *p = iface->properties ? iface->properties[i] : NULL;
        if (!p || p->type != AST_INTERFACE_PROPERTY)
            continue;
        InterfacePropertyNode *ip = &p->as.interface_property;
        if (!ip->name)
            continue;
        XaSymbol *psym = xa_symbol_new(ip->name, XA_SYM_PROPERTY);
        psym->location.line = p->line;
        XaSymbolLinks *plinks = xa_analyzer_get_links(ctx->analyzer, psym);
        plinks->type = ip->prop_type ? xr_tref_resolve_in_analyzer(ctx->analyzer, ip->prop_type)
                                     : xr_type_new_unknown(NULL);
        xa_class_info_add_field(info, psym);
    }
}

static bool xa_interface_signature_has_recovery_type(const XrType *type) {
    if (!type)
        return true;
    if (XR_TYPE_IS_UNKNOWN_OR_ERROR(type))
        return true;
    if (XR_TYPE_IS_FUNCTION(type)) {
        if (xa_interface_signature_has_recovery_type(type->function.return_type))
            return true;
        for (int i = 0; i < type->function.param_count; i++) {
            if (xa_interface_signature_has_recovery_type(xr_type_function_param_type(type, i)))
                return true;
        }
    }
    return false;
}

static XrType *xa_interface_required_signature(XaInferContext *ctx, XaSymbolLinks *iface_links,
                                               XrType *iface_type, XrType *signature) {
    if (!ctx || !ctx->analyzer || !signature || !iface_links || !iface_type)
        return signature;

    int type_param_count = xa_symbol_links_get_type_param_count(iface_links);
    if (type_param_count <= 0 || iface_type->instance.type_arg_count != type_param_count ||
        !iface_type->instance.type_args) {
        return signature;
    }

    const char *stack_names[8];
    const char **type_param_names =
        type_param_count <= 8 ? stack_names
                              : xr_malloc(sizeof(const char *) * (size_t) type_param_count);
    if (!type_param_names)
        return signature;

    for (int i = 0; i < type_param_count; i++) {
        type_param_names[i] = xa_symbol_links_get_type_param_name(iface_links, i);
    }

    XrType *result = xr_type_substitute(ctx->analyzer->isolate, signature, type_param_names,
                                        iface_type->instance.type_args, type_param_count);
    if (type_param_names != stack_names)
        xr_free((void *) type_param_names);
    return result ? result : signature;
}

// Verify that `cls_info` provides every method/property required by every
// user-defined interface listed in info->interface_types. Built-in interface
// conformance (Iterable / Comparable / ...) is checked by
// xr_type_satisfies_constraint and stays outside this loop.
static void xa_check_interface_conformance(XaInferContext *ctx, AstNode *cls_node,
                                           XrClassInfo *cls_info) {
    if (!cls_info || cls_info->interface_count == 0 || !cls_info->interface_types)
        return;

    for (int i = 0; i < cls_info->interface_count; i++) {
        XrType *iface_type = cls_info->interface_types[i];
        if (!iface_type)
            continue;

        const char *iface_name = iface_type->instance.class_name;
        if (!iface_name)
            continue;

        // Built-in interfaces have no XrClassInfo* attached. Hashable is the
        // one builtin with a user-visible structural contract.
        //
        // Identity, not spelling, decides which is which: `interface Lengthable
        // { ... }` written in user code carries its own declaration info, and is
        // audited as an ordinary interface below against the methods it actually
        // declares — not against the builtin's `operator len() -> int` contract.
        const bool is_builtin_iface = iface_type->instance.class_ref == NULL;
        if (is_builtin_iface && strcmp(iface_name, "Hashable") == 0) {
            xa_validate_hashable_contract_for_class(ctx, cls_node, cls_info);
            continue;
        }
        if (is_builtin_iface && xa_is_builtin_interface_name(iface_name)) {
            if (strcmp(iface_name, "Lengthable") == 0) {
                XaSymbol *found = xa_class_info_lookup_member(cls_info, "__operator_len");
                XaSymbolLinks *found_links =
                    found ? xa_analyzer_get_links(ctx->analyzer, found) : NULL;
                XrType *signature = found_links ? found_links->type : NULL;
                bool valid = found && found->kind == XA_SYM_METHOD && !found->is_static &&
                             found->receiver_mode == XR_PARAM_READ && signature &&
                             signature->kind == XR_KIND_FUNCTION &&
                             signature->function.param_count == 0 &&
                             signature->function.return_type &&
                             signature->function.return_type->kind == XR_KIND_INT &&
                             !signature->function.return_type->is_nullable;
                if (!valid) {
                    char msg[320];
                    snprintf(msg, sizeof(msg),
                             "Class '%s' implements Lengthable but does not provide a valid "
                             "non-mutating 'operator len() -> i64'",
                             cls_info->name ? cls_info->name : "?");
                    XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
                }
                continue;
            }
            /* Every other builtin interface declares its contract in the
             * method table; a declared implementation is held to it the same
             * way user interfaces are. Types stay unchecked here (generic
             * slots have no concrete instantiation at this point); presence,
             * instance-ness and arity already catch the silent no-op case. */
            const XaInterfaceDefinition *def = xa_builtin_interface_definition(iface_name);
            for (int j = 0; def && def->methods && j < def->method_count; j++) {
                const XaInterfaceMethod *required = &def->methods[j];
                if (!required->name)
                    continue;
                XaSymbol *found = xa_class_info_lookup_member(cls_info, required->name);
                XaSymbolLinks *found_links =
                    found ? xa_analyzer_get_links(ctx->analyzer, found) : NULL;
                XrType *signature = found_links ? found_links->type : NULL;
                bool valid = found && found->kind == XA_SYM_METHOD && !found->is_static &&
                             signature && signature->kind == XR_KIND_FUNCTION &&
                             signature->function.param_count == required->param_count;
                if (!valid) {
                    char msg[320];
                    snprintf(msg, sizeof(msg),
                             "Class '%s' does not implement method '%s' required by interface "
                             "'%s'",
                             cls_info->name ? cls_info->name : "?", required->name, iface_name);
                    XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
                }
            }
            continue;
        }

        // Iterator is the one spec-level interface represented as a sealed
        // native class, so it never resolves through the interface path.
        // Hold implementors to its three-method protocol here.
        if (strcmp(iface_name, "Iterator") == 0) {
            static const struct {
                const char *name;
                int param_count;
            } iterator_protocol[] = {{"hasNext", 0}, {"next", 0}, {"nth", 1}};
            for (size_t j = 0; j < sizeof(iterator_protocol) / sizeof(iterator_protocol[0]); j++) {
                XaSymbol *found = xa_class_info_lookup_member(cls_info, iterator_protocol[j].name);
                XaSymbolLinks *fl = found ? xa_analyzer_get_links(ctx->analyzer, found) : NULL;
                XrType *signature = fl ? fl->type : NULL;
                bool valid = found && found->kind == XA_SYM_METHOD && !found->is_static &&
                             signature && signature->kind == XR_KIND_FUNCTION &&
                             signature->function.param_count == iterator_protocol[j].param_count;
                if (!valid) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Class '%s' does not implement method '%s' required by interface "
                             "'Iterator'",
                             cls_info->name ? cls_info->name : "?", iterator_protocol[j].name);
                    XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
                }
            }
            continue;
        }

        XaSymbol *iface_sym = xa_scope_lookup(ctx->analyzer->current_scope, iface_name);
        if (!iface_sym || iface_sym->kind != XA_SYM_CLASS)
            continue;
        XaSymbolLinks *iface_links = xa_analyzer_get_links(ctx->analyzer, iface_sym);
        if (!iface_links || !iface_links->class_info)
            continue;
        XrClassInfo *iface_info = iface_links->class_info;
        if (iface_links->type && iface_links->type->kind != XR_KIND_INTERFACE) {
            // Anything else that resolves to a non-interface is a user
            // error; skipping it silently made the clause look verified
            // when nothing was checked.
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "'%s' is not an interface; only interfaces can appear in an implements "
                     "clause",
                     iface_name);
            XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
            continue;
        }

        // Required methods
        for (int j = 0; j < iface_info->method_count; j++) {
            XaSymbol *required = iface_info->methods[j];
            if (!required || !required->name)
                continue;
            XaSymbol *found = xa_class_info_lookup_member(cls_info, required->name);
            if (!found || found->kind != XA_SYM_METHOD) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Class '%s' does not implement method '%s' required by interface '%s'",
                         cls_info->name ? cls_info->name : "?", required->name, iface_name);
                XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
                continue;
            }

            XaSymbolLinks *required_links = xa_analyzer_get_links(ctx->analyzer, required);
            XaSymbolLinks *found_links = xa_analyzer_get_links(ctx->analyzer, found);
            XrType *required_sig = xa_interface_required_signature(
                ctx, iface_links, iface_type, required_links ? required_links->type : NULL);
            XrType *found_sig = found_links ? found_links->type : NULL;
            if (!xa_interface_signature_has_recovery_type(required_sig) &&
                !xa_interface_signature_has_recovery_type(found_sig) &&
                !xr_type_function_signature_assignable(required_sig, found_sig)) {
                char msg[512];
                if (required_sig->function.receiver_mode != found_sig->function.receiver_mode) {
                    snprintf(msg, sizeof(msg),
                             "Class '%s' method '%s' does not match signature required by "
                             "interface '%s': expected '%s' with %s receiver, found '%s' with "
                             "%s receiver",
                             cls_info->name ? cls_info->name : "?", required->name, iface_name,
                             xr_type_to_string(required_sig),
                             xr_param_mode_label(required_sig->function.receiver_mode),
                             xr_type_to_string(found_sig),
                             xr_param_mode_label(found_sig->function.receiver_mode));
                } else {
                    snprintf(msg, sizeof(msg),
                             "Class '%s' method '%s' does not match signature required by "
                             "interface '%s': expected '%s', found '%s'",
                             cls_info->name ? cls_info->name : "?", required->name, iface_name,
                             xr_type_to_string(required_sig), xr_type_to_string(found_sig));
                }
                XrLocation loc = {.file = ctx->file_path, .line = found->location.line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
            }
        }

        // Required properties — accept either a plain field/property or an
        // accessor pair. Computed properties on the class side are stored as
        // methods named "get:<prop>" / "set:<prop>" (see xparse_oop), so look
        // up both shapes before reporting a missing member.
        for (int j = 0; j < iface_info->field_count; j++) {
            XaSymbol *required = iface_info->fields[j];
            if (!required || !required->name)
                continue;
            XaSymbol *found = xa_class_info_lookup_member(cls_info, required->name);
            if (!found) {
                char getter_name[128];
                snprintf(getter_name, sizeof(getter_name), "get:%s", required->name);
                found = xa_class_info_lookup_member(cls_info, getter_name);
            }
            if (!found) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Class '%s' does not provide property '%s' required by interface '%s'",
                         cls_info->name ? cls_info->name : "?", required->name, iface_name);
                XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
            }
        }
    }
}

/* The initial structural conformance check runs before error-effect fixpoint,
 * while implementation method types are deliberately POLY. Recheck only the
 * covariant throw-effect dimension after Pass 3 has published final bits. */
static void xa_validate_class_interface_throw_effects(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node ||
        (node->type != AST_CLASS_DECL && node->type != AST_STRUCT_DECL &&
         node->type != AST_UNION_DECL))
        return;
    ClassDeclNode *cls = node->type == AST_CLASS_DECL    ? &node->as.class_decl
                         : node->type == AST_STRUCT_DECL ? &node->as.struct_decl
                                                         : &node->as.union_decl;
    XaSymbol *class_symbol =
        cls->symbol_id ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, cls->symbol_id) : NULL;
    XrClassInfo *class_info = class_symbol ? class_symbol->links.class_info : NULL;
    if (!class_info || !class_info->interface_types)
        return;

    for (int i = 0; i < class_info->interface_count; i++) {
        XrType *interface_type = class_info->interface_types[i];
        const char *interface_name = interface_type ? interface_type->instance.class_name : NULL;
        if (!interface_name || xa_is_builtin_interface_name(interface_name))
            continue;
        XaSymbol *interface_symbol = xa_scope_lookup(ctx->analyzer->current_scope, interface_name);
        if (!interface_symbol)
            interface_symbol = xa_analyzer_lookup_deep(ctx->analyzer, interface_name);
        XaSymbolLinks *interface_links =
            interface_symbol ? xa_analyzer_get_links(ctx->analyzer, interface_symbol) : NULL;
        XrClassInfo *interface_info = interface_links ? interface_links->class_info : NULL;
        if (!interface_info ||
            (interface_links->type && interface_links->type->kind != XR_KIND_INTERFACE))
            continue;
        for (int j = 0; j < interface_info->method_count; j++) {
            XaSymbol *required = interface_info->methods[j];
            XaSymbol *found = required && required->name
                                  ? xa_class_info_lookup_member(class_info, required->name)
                                  : NULL;
            if (!required || !found || found->kind != XA_SYM_METHOD)
                continue;
            XrType *required_signature = xa_interface_required_signature(
                ctx, interface_links, interface_type, required->links.type);
            XrType *found_signature = found->links.type;
            if (!required_signature || !found_signature ||
                required_signature->kind != XR_KIND_FUNCTION ||
                found_signature->kind != XR_KIND_FUNCTION)
                continue;
            if (required_signature->function.return_type &&
                XR_TYPE_IS_SLICE(required_signature->function.return_type) &&
                !xr_type_function_view_origins_equal(required_signature, found_signature)) {
                char message[512];
                snprintf(message, sizeof(message),
                         "Class '%s' method '%s' borrowed-result origin set does not match "
                         "interface '%s'",
                         class_info->name ? class_info->name : "?", required->name, interface_name);
                XrLocation location = {.file = ctx->file_path, .line = found->location.line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, message,
                                           &location);
            }
            if (required_signature->function.throw_effect != XR_FN_EFFECT_NO_THROW ||
                found_signature->function.throw_effect == XR_FN_EFFECT_NO_THROW)
                continue;
            char message[512];
            snprintf(message, sizeof(message),
                     "Class '%s' method '%s' may throw but interface '%s' requires signature '%s'",
                     class_info->name ? class_info->name : "?", required->name, interface_name,
                     xr_type_to_string(required_signature));
            XrLocation location = {.file = ctx->file_path, .line = found->location.line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, message,
                                       &location);
        }
    }
}

void xa_validate_interface_throw_effects(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;
    if (node->type == AST_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++)
            xa_validate_interface_throw_effects(ctx, node->as.program.statements[i]);
        return;
    }
    xa_validate_class_interface_throw_effects(ctx, node);
}

/* A declaration may not take a name the language provides without an import.
 *
 * Shadowing was the old rule and it was never coherent. The user's declaration
 * won for constructors and instance methods but lost everywhere else: a user
 * class declaring `static withCapacity` still ran Array's, `Json.parse` on a
 * user Json compiled and then panicked at run time, and annotating a variable
 * produced "Type 'Array<i64>' is not assignable to type 'Array<i64>'" because
 * the two distinct types print the same. A native handle collision was worse
 * still and already rejected here -- that check is what this generalizes.
 *
 * Returns true when it reported, so the caller stops rather than registering a
 * symbol that would fight the builtin for the rest of the compilation. */
bool xa_reject_builtin_name_redeclaration(XaInferContext *ctx, AstNode *node,
                                          const char *decl_label, const char *name) {
    if (!ctx || !ctx->analyzer || !node || !name)
        return false;
    const char *handle_module = xa_builtin_find_handle_module(name);
    /* The stdlib is where builtins are defined, so a reserved name there is the
     * declaration the name refers to. A handle collision is still rejected
     * everywhere: that one is a layout mismatch, not a naming question. */
    if (!handle_module &&
        (!xa_builtin_name_is_reserved(name) || xa_analyzer_path_is_stdlib(ctx->file_path)))
        return false;

    char msg[256];
    if (handle_module) {
        snprintf(msg, sizeof(msg),
                 "%s '%s' conflicts with the builtin native handle type '%s.%s' — "
                 "choose a different name",
                 decl_label, name, handle_module, name);
    } else {
        snprintf(msg, sizeof(msg),
                 "%s '%s' redeclares the builtin '%s' — builtin names are reserved, "
                 "choose a different name",
                 decl_label, name, name);
    }
    XrLocation loc = {.file = ctx->file_path, .line = node->line};
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg, &loc);
    return true;
}

void xa_visit_collect_class(XaInferContext *ctx, AstNode *node) {
    if (!node)
        return;

    bool is_struct_decl = node->type == AST_STRUCT_DECL;
    bool is_union_decl = node->type == AST_UNION_DECL;
    bool is_aggregate_decl = is_struct_decl || is_union_decl;
    const char *decl_label = is_union_decl ? "union" : is_struct_decl ? "struct" : "class";
    const char *diag_label = is_union_decl ? "Union" : is_struct_decl ? "Struct" : "Class";
    ClassDeclNode *cls = (node->type == AST_CLASS_DECL) ? &node->as.class_decl
                         : is_struct_decl               ? &node->as.struct_decl
                                                        : &node->as.union_decl;

    if (xa_reject_builtin_name_redeclaration(ctx, node, decl_label, cls->name))
        return;

    XaSymbol *sym =
        cls->symbol_id ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, cls->symbol_id) : NULL;
    if (!sym && cls->name)
        sym = xa_scope_lookup_local(ctx->analyzer->current_scope, cls->name);
    if (!sym) {
        sym = xa_symbol_new(cls->name, XA_SYM_CLASS);
        sym->location.line = node->line;
        sym->is_exported = node->is_exported;
        xa_visit_add_symbol_checked(ctx, sym, 0);
        cls->symbol_id = sym->id;
    }
    sym->is_exported = node->is_exported;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    const char *stdlib_module = xa_intrinsic_owner_module(ctx);
    if (links && stdlib_module) {
        /* Preserve canonical stdlib provenance past analysis.  Lowering uses
         * this identity to attach generated provider-bridge layout metadata;
         * a source filename or bare class spelling is not sufficient. */
        links->module_name = stdlib_module;
        links->file_path = ctx->file_path;
    }
    xa_publish_deprecated_attrs(links, cls->attributes, cls->attr_count);
    XrClassInfo *info = links ? links->class_info : NULL;
    const char *capability_name = cls->generic_origin_name ? cls->generic_origin_name : cls->name;
    /* The hoisting collector and direct declaration visitor can both reach the
     * same declaration.  Once the class scope exists, its fields/methods and
     * layout are already complete; collecting them again duplicates physical
     * fields and silently doubles struct size. */
    if (info && info->scope) {
        info->capability_flags |=
            xa_declared_type_capability_flags(ctx->file_path, capability_name);
        /* Deferred-layout recompute: a fixed-layout aggregate that references a
         * generic value struct (e.g. a `Box<int>` field) is collected during
         * the first analysis pass, before the mono pass materializes the
         * concrete `Box$i64`. Its layout was deferred (left NULL). Now that the
         * concrete instance exists, re-collect the aggregate fresh so the layout
         * is built against the concrete nested layout. Resetting the member
         * counts keeps the arrays/maps but lets the re-collection overwrite them
         * (member_list_append + hashmap_set are index-0/overwrite based). */
        bool is_nongeneric_aggregate =
            is_aggregate_decl && (!is_struct_decl || node->as.struct_decl.type_param_count == 0);
        /* Post-mono re-collect: the mono pass rewrote at least one member type
         * annotation under this declaration (e.g. a `Box<int>` constructor
         * parameter is now `Box$i64`). The members collected during the first
         * analysis pass still carry the pre-mono generic instance type, so every
         * call checked against them would mismatch the monomorphized argument. */
        if ((is_nongeneric_aggregate && !info->struct_layout && info->field_count > 0) ||
            cls->mono_types_rewritten) {
            info->scope = NULL;
            info->field_count = 0;
            info->method_count = 0;
            info->static_field_count = 0;
            info->static_method_count = 0;
            cls->mono_types_rewritten = false;
            xa_visit_collect_class(ctx, node);
        }
        return;
    }
    if (!info) {
        info = xa_class_info_new(cls->name);
        info->explicit_final = cls->explicit_final;
        info->derive_flags = xa_class_decl_derive_flags(cls->attributes, cls->attr_count);
        info->capability_flags = xa_declared_type_capability_flags(ctx->file_path, capability_name);
        info->location =
            (XrLocation) {.file = ctx->file_path, .line = node->line, .column = node->column};
        if (cls->super_name) {
            info->base_name = xr_strdup(cls->super_name);
        }
        links->class_info = info;
        links->owns_class_info = true;
    } else {
        info->explicit_final = cls->explicit_final;
        info->derive_flags = xa_class_decl_derive_flags(cls->attributes, cls->attr_count);
        info->capability_flags = xa_declared_type_capability_flags(ctx->file_path, capability_name);
        info->location =
            (XrLocation) {.file = ctx->file_path, .line = node->line, .column = node->column};
    }
    if (!links->type) {
        links->type = xr_type_new_class(ctx->analyzer->isolate, cls->name);
    }
    links->type->instance.class_ref = info;
    if (is_aggregate_decl) {
        links->type->is_value_type = true;
    }

    if (is_union_decl) {
        if (cls->type_param_count > 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH,
                                       "Union declarations cannot be generic", &loc);
        }
        if (cls->interface_count > 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH,
                                       "Union declarations cannot implement interfaces", &loc);
        }
        if (cls->method_count > 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH,
                                       "Union declarations cannot declare methods", &loc);
        }
    }

    // Resolve every entry in the 'implements' clause to a runtime XrType
    // so constraint checks and conformance lookups can compare type
    // arguments structurally instead of falling back to bare-name matches.
    if (cls->interface_count > 0 && cls->interfaces) {
        info->interface_types = xr_malloc(sizeof(XrType *) * cls->interface_count);
        if (!info->interface_types)
            goto skip_interfaces;
        info->interface_count = cls->interface_count;
        for (int i = 0; i < cls->interface_count; i++) {
            info->interface_types[i] =
                xr_tref_resolve_in_analyzer(ctx->analyzer, cls->interfaces[i]);
        }
    }

skip_interfaces:

    // Store generic type parameters and intersection-style constraint lists.
    xa_store_type_params_with_constraints(ctx, links, cls->type_params, cls->type_param_count,
                                          node);

    // Enter class scope
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
    ctx->analyzer->current_scope->class_symbol = sym;
    info->scope = ctx->analyzer->current_scope;

    // Collect fields
    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field = cls->fields[i];
        if (field && field->type == AST_FIELD_DECL) {
            FieldDeclNode *fd = &field->as.field_decl;
            XaSymbol *field_sym = xa_symbol_new(fd->name, XA_SYM_PROPERTY);
            field_sym->location.line = field->line;
            field_sym->is_static = fd->is_static;
            field_sym->is_private = fd->is_private;
            field_sym->is_protected = fd->is_protected;
            field_sym->is_const = fd->is_const;
            field_sym->is_weak = fd->is_weak;
            field_sym->has_declared_default = (fd->initializer != NULL);
            xa_visit_add_symbol_checked(ctx, field_sym, 0);

            XaSymbolLinks *field_links = xa_analyzer_get_links(ctx->analyzer, field_sym);

            // Try explicit type annotation first
            if (fd->field_type) {
                field_links->type = fd->field_type
                                        ? xr_tref_resolve_in_analyzer(ctx->analyzer, fd->field_type)
                                        : xr_type_new_unknown(NULL);
            } else if (fd->initializer) {
                // Infer type from initializer
                field_links->type =
                    xa_function_value_storage_type(ctx, xa_visit_infer(ctx, fd->initializer));
            } else {
                field_links->type = xr_type_new_unknown(NULL);
                // Warn: class field missing type annotation and initializer
                char msg[256];
                snprintf(
                    msg, sizeof(msg),
                    "Field '%s' is missing type annotation (and has no initializer to infer from)",
                    fd->name ? fd->name : "?");
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
            }
            /* `weak` rules W2 and W3 (spec 16.3). W4 (EXEC_LOCAL only) is
             * checked where the object's storage domain is decided, not here —
             * a field declaration does not know it yet. */
            if (fd->is_weak) {
                const char *why = NULL;
                XrType *wt = field_links->type;
                if (fd->is_static) {
                    /* A static field belongs to the module, not to any
                     * coroutine-local instance, so nothing would ever run the
                     * clearing hook that makes W5 true. */
                    why = "a static field cannot be weak: nothing would clear it";
                } else if (!wt || wt->kind == XR_KIND_UNKNOWN) {
                    why = "a weak field needs an explicit nullable type";
                } else if (!wt->is_nullable) {
                    /* W2. A weak slot reads null the instant its target dies;
                     * a non-nullable declaration would be a lie the type system
                     * could not catch anywhere else. */
                    why = "a weak field must be nullable — write `weak name: T?`";
                } else if (!xa_type_can_be_weak(wt)) {
                    /* Nothing to be weak about: a scalar has no refcount, so
                     * `weak` on it would silently mean nothing. */
                    why = "only a reference type can be weak";
                }
                if (why) {
                    XrLocation loc = {.file = ctx->file_path, .line = field->line};
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s (field '%s')", why, fd->name ? fd->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_WEAK_FIELD, msg, &loc);
                }
            }

            if (xa_type_contains_span_view(field_links->type)) {
                XrLocation loc = {.file = ctx->file_path, .line = field->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot store Slice view in %s field '%s'; Slice is a borrowed view and "
                         "cannot live in long-lived storage",
                         decl_label ? decl_label : "class", fd->name ? fd->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_BORROW_ESCAPE, msg, &loc);
            }
            xa_class_info_add_field(info, field_sym);
        }
    }

    // Check fixed-layout aggregate constraints before layout construction.
    bool struct_field_types_valid = true;
    if (is_aggregate_decl) {
        for (int i = 0; i < info->field_count; i++) {
            XaSymbol *fs = info->fields[i];
            if (!fs)
                continue;
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fs);
            if (!fl || !fl->type)
                continue;
            XrType *ft = fl->type;

            AstNode *field_node = (i < cls->field_count) ? cls->fields[i] : NULL;
            FieldDeclNode *field_decl = field_node && field_node->type == AST_FIELD_DECL
                                            ? &field_node->as.field_decl
                                            : NULL;
            if (field_decl && field_decl->is_flexible) {
                XrLocation loc = {.file = ctx->file_path, .line = field_node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH,
                                           "flexible array fields were removed; use a byte "
                                           "accessor or a manifest-declared mechanical ABI shim",
                                           &loc);
                struct_field_types_valid = false;
                continue;
            }

            if (ft->kind == XR_KIND_ARRAY || ft->kind == XR_KIND_MAP || ft->kind == XR_KIND_SET) {
                XrLocation loc = {.file = ctx->file_path, .line = fs->location.line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "%s '%s' field '%s' cannot use a dynamic container type; "
                         "use a class field or a fixed array [T; N]",
                         diag_label, cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                struct_field_types_valid = false;
                continue;
            }

            if (is_union_decl) {
                AstNode *field_node = (i < cls->field_count) ? cls->fields[i] : NULL;
                if (field_node && field_node->type == AST_FIELD_DECL &&
                    field_node->as.field_decl.initializer) {
                    XrLocation loc = {.file = ctx->file_path, .line = field_node->line};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Union '%s' field '%s' cannot have a default initializer",
                             cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    struct_field_types_valid = false;
                    continue;
                }
            }

            if (!cls->name)
                continue;
            // Field referencing the same struct → infinite size
            const char *type_name = NULL;
            if ((ft->kind == XR_KIND_CLASS || ft->kind == XR_KIND_INSTANCE) &&
                ft->instance.class_name) {
                type_name = ft->instance.class_name;
            }
            if (type_name && strcmp(type_name, cls->name) == 0) {
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "%s '%s' cannot have a field of its own type — "
                         "this creates infinite size. Use a class instead for recursive data",
                         diag_label, cls->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                struct_field_types_valid = false;
            }
        }
    }

    // Compute fixed-layout aggregate layout (VALUE_TYPE only, skip generic struct templates)
    int struct_type_param_count = is_struct_decl ? node->as.struct_decl.type_param_count : 0;
    /* Tell the native plan this program declares the type, before deciding
     * whether it gets a fixed layout.  A [[native.layout]] assertion naming a
     * type that no compiled module declares is vacuous for this build; one
     * naming a type that IS declared but has no fixed layout is a real error.
     * Without this signal the two are indistinguishable. */
    if (is_aggregate_decl && cls->name) {
        XrNativePackagePlan *layout_subject_plan =
            (XrNativePackagePlan *) xr_compiler_session_native_package_plan(
                ctx->analyzer ? ctx->analyzer->compiler_session : NULL);
        if (layout_subject_plan)
            xr_native_package_note_layout_subject(layout_subject_plan, cls->name);
    }
    if (is_aggregate_decl && info->field_count > 0 && struct_type_param_count == 0 &&
        struct_field_types_valid) {
        XrAggregateLayout *layout = xr_calloc(1, sizeof(XrAggregateLayout));
        if (!layout)
            goto skip_layout;
        layout->nominal_name = cls->name ? xr_strdup(cls->name) : NULL;
        if (cls->name && !layout->nominal_name) {
            xr_free(layout);
            goto skip_layout;
        }
        layout->field_count = (uint16_t) info->field_count;
        ClassDeclNode *st = is_union_decl ? &node->as.union_decl : &node->as.struct_decl;
        if (is_union_decl) {
            layout->kind = XR_AGG_LAYOUT_UNION;
        } else if (st->is_packed) {
            layout->kind = XR_AGG_LAYOUT_PACKED_STRUCT;
        } else {
            layout->kind = XR_AGG_LAYOUT_STRUCT;
        }
        layout->explicit_align = st->explicit_align;
        bool layout_valid = true;
        if (layout->explicit_align != 0 &&
            (layout->explicit_align & (layout->explicit_align - 1)) != 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line};
            char msg[256];
            snprintf(msg, sizeof(msg), "%s '%s' align value must be a power of two", diag_label,
                     cls->name ? cls->name : "?");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            layout_valid = false;
        }
        /* Aggregate layouts outlive individual symbol-table views (IR types,
         * bytecode descriptors and AOT planning may retain the layout after
         * analyzer teardown), so field names are layout-owned copies. */
        layout->field_names = xr_calloc((size_t) info->field_count, sizeof(const char *));
        if (!layout->field_names) {
            xr_aggregate_layout_free_owned(layout);
            goto skip_layout;
        }
        for (int i = 0; i < info->field_count; i++) {
            const char *name = info->fields[i] ? info->fields[i]->name : NULL;
            layout->field_names[i] = name ? xr_strdup(name) : NULL;
            if (name && !layout->field_names[i]) {
                layout_valid = false;
                break;
            }
        }

        for (int i = 0; i < info->field_count && i < XR_MAX_AGG_FIELDS; i++) {
            if (!layout_valid)
                break;
            XaSymbol *fs = info->fields[i];
            if (!fs) {
                layout_valid = false;
                break;
            }
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fs);
            XrType *ft = (fl && fl->type) ? fl->type : NULL;
            if (!ft || ft->kind == XR_KIND_UNKNOWN) {
                // Phase 1: struct fields must have explicit type annotations
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "%s '%s' field '%s' must have an explicit type annotation "
                         "(i64, f64, bool, string, fixed array, or struct)",
                         diag_label, cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                layout_valid = false;
                break;
            }

            int native = xr_type_kind_to_native(ft->kind, ft->scalar_rep);
            if (native < 0) {
                // Fixed-size array field: [T; N]
                if (ft->kind == XR_KIND_FIXED_ARRAY && ft->fixed_array.element_type) {
                    XrType *elem = ft->fixed_array.element_type;
                    int elem_native = xa_fixed_array_elem_native_lane(elem);
                    if (ft->fixed_array.length <= 0) {
                        XrLocation loc = {.file = ctx->file_path, .line = node->line};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "%s '%s' field '%s': fixed array length must be positive",
                                 diag_label, cls->name ? cls->name : "?",
                                 fs->name ? fs->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                    uint32_t elem_size = xr_native_type_size(
                        xa_analyzer_target_data_layout(ctx->analyzer), (uint8_t) elem_native);
                    uint64_t field_bytes = (uint64_t) ft->fixed_array.length * (uint64_t) elem_size;
                    if (elem_size == 0 || field_bytes > UINT16_MAX ||
                        (uint32_t) ft->fixed_array.length > UINT16_MAX) {
                        XrLocation loc = {.file = ctx->file_path, .line = node->line};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "Fixed array field '%s' in struct '%s' exceeds maximum size "
                                 "(%u bytes > 65535). For larger collections, use a class with "
                                 "Array<T>.",
                                 fs->name ? fs->name : "?", cls->name ? cls->name : "?",
                                 (unsigned) (field_bytes > UINT32_MAX ? UINT32_MAX : field_bytes));
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                    layout->fields[i].native_type = XR_NATIVE_ARRAY;
                    layout->fields[i].elem_native_type = (uint8_t) elem_native;
                    layout->fields[i].elem_count = (uint16_t) ft->fixed_array.length;
                    if (is_union_decl &&
                        !xa_struct_field_bitwise_reinterpretable(&layout->fields[i])) {
                        XrLocation loc = {.file = ctx->file_path, .line = fs->location.line};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "Union '%s' field '%s' must be bitwise-reinterpretable",
                                 cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                    continue;
                }
                // Check if field type is a nested struct with known layout
                const char *field_class_name = NULL;
                if ((ft->kind == XR_KIND_CLASS || ft->kind == XR_KIND_INSTANCE) &&
                    ft->instance.class_name) {
                    field_class_name = ft->instance.class_name;
                }
                XrAggregateLayout *sub_layout = NULL;
                if (field_class_name) {
                    XaSymbol *sub_sym = xa_analyzer_lookup(ctx->analyzer, field_class_name);
                    if (sub_sym) {
                        XaSymbolLinks *sub_links = xa_analyzer_get_links(ctx->analyzer, sub_sym);
                        if (sub_links && sub_links->class_info &&
                            sub_links->class_info->struct_layout) {
                            sub_layout = sub_links->class_info->struct_layout;
                        }
                    }
                }
                if (sub_layout) {
                    layout->fields[i].native_type = XR_NATIVE_NESTED_AGGREGATE;
                    layout->fields[i].size =
                        (uint16_t) xr_aggregate_layout_storage_size(sub_layout);
                    layout->fields[i].sub_layout_id = sub_layout->layout_id;
                    layout->fields[i].sub_layout = sub_layout;
                    if (is_union_decl &&
                        !xa_struct_field_bitwise_reinterpretable(&layout->fields[i])) {
                        XrLocation loc = {.file = ctx->file_path, .line = fs->location.line};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "Union '%s' field '%s' must be bitwise-reinterpretable",
                                 cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                } else if (xa_field_is_pending_generic_value_struct(ctx, ft, field_class_name)) {
                    /* Generic value-struct field (e.g. `Box<int>`) whose concrete
                     * monomorphized instance is not registered yet: the mono pass
                     * runs after this first analysis. Defer the aggregate layout
                     * (discard the partial layout silently, no diagnostic). The
                     * post-mono re-analysis re-collects this struct once the
                     * concrete `Box$i64` layout exists — see the deferred-layout
                     * recompute at the top of xa_visit_collect_class. */
                    layout_valid = false;
                    break;
                } else {
                    XrLocation loc = {.file = ctx->file_path, .line = node->line};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "%s '%s' field '%s' has unsupported type — "
                             "only scalar values, raw pointers, fixed arrays and other structs are "
                             "supported",
                             diag_label, cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    layout_valid = false;
                    break;
                }
                continue;
            }

            layout->fields[i].native_type = (uint8_t) native;
            if (is_union_decl && !xa_struct_field_bitwise_reinterpretable(&layout->fields[i])) {
                XrLocation loc = {.file = ctx->file_path, .line = fs->location.line};
                char msg[256];
                snprintf(msg, sizeof(msg), "Union '%s' field '%s' must be bitwise-reinterpretable",
                         cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                layout_valid = false;
                break;
            }
        }

        if (layout_valid && info->field_count <= XR_MAX_AGG_FIELDS) {
            if (!xr_aggregate_layout_compute(layout,
                                             xa_analyzer_target_data_layout(ctx->analyzer))) {
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH,
                                           "Cannot compute aggregate layout for target ABI", &loc);
                xr_aggregate_layout_free_owned(layout);
                layout = NULL;
            } else if (layout->total_size > UINT16_MAX) {
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "%s '%s' total size exceeds maximum (%u bytes > 65535). "
                         "For larger data, use a class with Array<T> fields.",
                         diag_label, cls->name ? cls->name : "?", (unsigned) layout->total_size);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                xr_aggregate_layout_free_owned(layout);
            } else {
                info->struct_layout = layout;
                XrNativePackagePlan *native_plan =
                    (XrNativePackagePlan *) xr_compiler_session_native_package_plan(
                        ctx->analyzer ? ctx->analyzer->compiler_session : NULL);
                if (native_plan)
                    (void) xr_native_package_resolve_layout(native_plan, cls->name, layout);
            }
        } else {
            xr_aggregate_layout_free_owned(layout);
        }
    }
skip_layout:

    // Collect methods
    for (int i = 0; !is_union_decl && i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (method && method->type == AST_METHOD_DECL) {
            MethodDeclNode *md = &method->as.method_decl;
            /* A duplicate method name is rejected wholesale: two bodies under
             * one name make the error-set fixpoint oscillate. The duplicate
             * neither registers a symbol nor an analyzable body. */
            bool duplicate = false;
            for (int j = 0; j < i; j++) {
                AstNode *prior = cls->methods[j];
                if (prior && prior->type == AST_METHOD_DECL && prior->as.method_decl.name &&
                    md->name && strcmp(prior->as.method_decl.name, md->name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Symbol '%s' is redefined in the same scope",
                         md->name ? md->name : "?");
                XrLocation loc = {
                    .file = ctx->file_path, .line = method->line, .column = method->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_CMP_REDEFINED_VAR, msg, &loc);
                continue;
            }
            XaSymbol *method_sym = xa_symbol_new(md->name, XA_SYM_METHOD);
            method_sym->location.line = method->line;
            method_sym->is_static = md->is_static;
            method_sym->is_private = md->is_private;
            method_sym->is_protected = md->is_protected;
            method_sym->receiver_mode = md->is_static ? XR_PARAM_READ : md->receiver_mode;
            if (node->type == AST_STRUCT_DECL && method_sym->receiver_mode == XR_PARAM_MOVE) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = method->line, .column = method->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                    "struct methods cannot declare a MOVE receiver because structs are copy "
                    "values without an ownership root",
                    &loc);
            }
            xa_visit_add_symbol_checked(ctx, method_sym, 0);
            XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
            xa_publish_deprecated_attrs(method_links, md->attributes, md->attr_count);
            XaScope *signature_scope = ctx->analyzer ? ctx->analyzer->current_scope : NULL;
            XaSymbol *saved_signature_function =
                signature_scope ? signature_scope->function_symbol : NULL;
            // A method carries its own generic params and constraints, resolved
            // here so the constraint-aware analyzer sites (call-site constraint
            // checks, the Hashable key check) see them while the signature and
            // the body are still being processed.
            if (md->type_param_count > 0 && md->type_params) {
                xa_store_type_params_with_constraints(ctx, method_links, md->type_params,
                                                      md->type_param_count, method);
                if (signature_scope)
                    signature_scope->function_symbol = method_sym;
            }

            // Build method type
            XrType **param_types = NULL;
            const char **param_names = NULL;
            if (md->param_count > 0) {
                param_types = xr_malloc(sizeof(XrType *) * md->param_count);
                param_names = xr_malloc(sizeof(char *) * md->param_count);
                if (!param_types || !param_names) {
                    xr_free(param_types);
                    xr_free(param_names);
                    param_types = NULL;
                    param_names = NULL;
                }
                for (int j = 0; param_types && j < md->param_count; j++) {
                    XrParamNode *param = md->params ? md->params[j] : NULL;
                    param_types[j] =
                        (param && param->type)
                            ? xr_tref_resolve_parameter_in_analyzer(ctx->analyzer, param->type)
                            : xr_type_new_unknown(NULL);
                    param_names[j] = param ? param->name : NULL;

                    // Warn: method parameter missing type annotation (skip constructor)
                    bool is_rest_param = param && param->is_rest;
                    if (!(param && param->type) && !md->is_constructor && !is_rest_param) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "Parameter '%s' of method '%s' is missing type annotation",
                                 param && param->name ? param->name : "?",
                                 md->name ? md->name : "?");
                        XrLocation loc = {.file = ctx->file_path, .line = method->line};
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                    }
                }
            }

            // Omitted return type defaults to void; error if body has 'return <expr>'
            // Skip getter/setter (set:xxx, get:xxx) - return types are implicit
            bool is_accessor = md->name && (strncmp(md->name, "set:", 4) == 0 ||
                                            strncmp(md->name, "get:", 4) == 0);
            XrType *ret_type = md->return_type
                                   ? xr_tref_resolve_in_analyzer(ctx->analyzer, md->return_type)
                                   : NULL;
            if (!ret_type && is_accessor && md->body) {
                ret_type = xa_infer_function_return_type(ctx, md->body);
            }
            if (!ret_type) {
                ret_type = xr_type_new_unit(NULL);
            }
            if (md->is_operator && md->op_type == OPTYPE_LEN) {
                bool valid = !md->is_static && md->param_count == 0 && ret_type &&
                             ret_type->kind == XR_KIND_INT && !ret_type->is_nullable;
                if (!valid) {
                    XrLocation loc = {.file = ctx->file_path, .line = method->line};
                    xa_analyzer_add_diagnostic(
                        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                        "operator len must be an instance operator with signature "
                        "'operator len() -> i64'",
                        &loc);
                }
            }
            if (!md->return_type && !md->is_constructor && !is_accessor && md->body) {
                if (xa_body_has_return_expr(md->body)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Method '%s' returns a value but has no return type annotation",
                             md->name ? md->name : "?");
                    XrLocation loc = {.file = ctx->file_path, .line = method->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                }
            }

            // Resolve CLASS("T") → TYPE_PARAM("T") for generic methods
            if (md->type_param_count > 0 && md->type_params) {
                const char **tp_names =
                    xr_malloc(sizeof(const char *) * (size_t) md->type_param_count);
                if (tp_names) {
                    for (int j = 0; j < md->type_param_count; j++)
                        tp_names[j] = md->type_params[j] ? md->type_params[j]->name : NULL;
                    for (int j = 0; j < md->param_count; j++) {
                        param_types[j] = resolve_class_to_type_param(NULL, param_types[j], tp_names,
                                                                     md->type_param_count);
                    }
                    ret_type =
                        resolve_class_to_type_param(NULL, ret_type, tp_names, md->type_param_count);
                    xr_free(tp_names);
                }
            }
            if (signature_scope)
                signature_scope->function_symbol = saved_signature_function;

            XrType *method_type = xr_type_new_function(ctx->analyzer->isolate, param_types,
                                                       md->param_count, ret_type, md->is_variadic);
            if (method_type) {
                method_type->function.min_params = md->required_count;
                method_type->function.receiver_mode = method_sym->receiver_mode;
                xr_type_function_set_throw_effect(method_type, md->body ? XR_FN_EFFECT_POLY
                                                                        : XR_FN_EFFECT_MAY_THROW);
            }

            if (method_type && md->params) {
                for (int j = 0; j < md->param_count; j++) {
                    XrParamNode *param = md->params[j];
                    xr_type_function_set_param_mode(method_type, j,
                                                    param ? param->passing_mode : XR_PARAM_READ);
                }
            }

            method_links->type = method_type;
            method_links->file_path = ctx->file_path;
            method_links->function_decl_node = method;

            // Store parameter info for LSP
            xa_symbol_links_set_function_sig(method_links, param_types, param_names,
                                             md->param_count, ret_type);
            xa_bind_declared_view_origins(ctx, method, method_links, param_names,
                                          !md->is_static && !md->is_constructor);
            xa_symbol_links_set_param_escape_summary(ctx, method_links, param_types, param_names,
                                                     md->param_count, ret_type, md->body, info);
            // Record method/constructor default expressions for caller-side default filling.
            if (md->param_count > 0) {
                AstNode **defs = (AstNode **) xr_calloc(md->param_count, sizeof(AstNode *));
                if (defs) {
                    for (int j = 0; j < md->param_count; j++)
                        defs[j] = md->params && md->params[j] ? md->params[j]->default_value : NULL;
                    xa_bind_param_default_exprs(ctx, defs, param_types, md->param_count);
                    xa_symbol_links_set_param_defaults(method_links, defs, md->param_count);
                    xr_free(defs);
                }
            }

            // Generic type params (and their constraints) were stored on
            // method_links above, before the signature was resolved.

            XrLocation sig_loc = {
                .file = ctx->file_path, .line = method->line, .column = method->column};
            for (int j = 0; j < md->param_count; j++) {
                xa_validate_hashable_key_type(ctx, xr_type_function_param_type(method_type, j),
                                              method_links, "method parameter type", &sig_loc);
            }
            xa_validate_hashable_key_type(ctx, method_type->function.return_type, method_links,
                                          "method return type", &sig_loc);

            const char *intrinsic_owner_name =
                cls->generic_origin_name ? cls->generic_origin_name : cls->name;
            xa_bind_registry_intrinsic(ctx, method, method_sym, intrinsic_owner_name, md->name,
                                       md->is_static, md->param_count);

            xa_class_info_add_method(info, method_sym);

            // Record constructor info in class_info and validate super() rules
            if (md->is_constructor) {
                info->has_constructor = true;
                info->constructor_param_count = md->param_count;
                // Count required params (those without default values)
                int required = 0;
                for (int j = 0; j < md->param_count; j++) {
                    if (!md->params || !md->params[j] || !md->params[j]->default_value)
                        required++;
                }
                info->constructor_required_params = required;
                validate_constructor_super_call(ctx, cls, md, method);
            }

            if (param_types)
                xr_free(param_types);
            if (param_names)
                xr_free(param_names);
        }
    }

    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *field = info->fields[i];
        XaSymbolLinks *field_links = field ? xa_analyzer_get_links(ctx->analyzer, field) : NULL;
        XrLocation loc = {
            .file = ctx->file_path, .line = field ? field->location.line : node->line, .column = 0};
        xa_validate_hashable_key_type(ctx, field_links ? field_links->type : NULL, links,
                                      "field type", &loc);
    }

    validate_class_field_default_initialization(ctx, node, cls, info);

    // Enter each method scope and add parameters + visit body for nested declarations.
    // This creates the function scopes that Pass 2 will reuse via ast_node matching,
    // ensuring method parameters are visible during type inference.
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (!md->body)
            continue;

        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, method);

        // Look up method symbol to get resolved param types
        XaSymbol *msym = xa_scope_lookup_local(ctx->analyzer->current_scope->parent, md->name);
        ctx->analyzer->current_scope->function_symbol = msym;
        XaSymbolLinks *mlinks = msym ? xa_analyzer_get_links(ctx->analyzer, msym) : NULL;

        if (!md->is_static) {
            XaSymbol *this_sym = xa_symbol_new("this", XA_SYM_PARAMETER);
            if (this_sym) {
                this_sym->location.line = method->line;
                this_sym->passing_mode = md->is_constructor ? XR_PARAM_REF : md->receiver_mode;
                xa_visit_add_symbol_checked(ctx, this_sym, 0);
                XaSymbolLinks *this_links = xa_analyzer_get_links(ctx->analyzer, this_sym);
                if (this_links) {
                    XrType *this_type =
                        xr_type_new_named_instance(ctx->analyzer->isolate, cls->name);
                    if (this_type) {
                        this_type->instance.class_ref = info;
                        this_type->is_value_type = node->type == AST_STRUCT_DECL;
                    }
                    this_links->type = this_type;
                    this_links->is_definitely_assigned = true;
                }
            }
        }

        for (int j = 0; j < md->param_count; j++) {
            XrParamNode *source_param = md->params ? md->params[j] : NULL;
            const char *pname = source_param ? source_param->name : NULL;
            if (!pname)
                continue;

            XaSymbol *param = xa_visit_bind_parameter_symbol(ctx, source_param, method->line);
            if (!param)
                continue;

            XaSymbolLinks *plinks = xa_analyzer_get_links(ctx->analyzer, param);
            if (plinks) {
                XrType *param_type = (mlinks && mlinks->param_types && j < mlinks->param_count)
                                         ? mlinks->param_types[j]
                                         : xr_type_new_unknown(NULL);
                if (md->is_variadic && j == md->param_count - 1) {
                    param_type = xr_type_new_array(ctx->analyzer->isolate, param_type);
                }
                plinks->type = param_type;
                plinks->is_definitely_assigned = true;
            }
        }

        // Visit body for nested declarations (variables, nested functions, etc.)
        if (md->body)
            xa_visit_collect(ctx, md->body);

        if (mlinks) {
            xa_symbol_links_set_param_escape_summary(ctx, mlinks, mlinks->param_types,
                                                     mlinks->param_names, mlinks->param_count,
                                                     mlinks->return_type, md->body, info);
            xa_validate_declared_view_origin_returns(ctx, mlinks, md->body, info);
        }

        /* Publish return ownership here, while the method's own scope is still
         * current -- the scan resolves returned names through the visible
         * scope, so computing it on demand from a later phase would resolve
         * them somewhere else or not at all. The member symbol is the one the
         * evidence producer reads, which is not always the symbol `mlinks`
         * came from. */
        {
            XaSymbol *member_sym = xa_class_info_lookup_member(info, md->name);
            XaSymbolLinks *member_links =
                member_sym ? xa_analyzer_get_links(ctx->analyzer, member_sym) : NULL;
            if (member_links)
                xa_ensure_function_return_ownership_prepass(ctx, member_links);
            if (mlinks && mlinks != member_links)
                xa_ensure_function_return_ownership_prepass(ctx, mlinks);
        }

        xa_analyzer_exit_scope(ctx->analyzer);
    }

    for (int pass = 0; pass < cls->method_count + 1; pass++) {
        bool changed = false;
        for (int i = 0; i < cls->method_count; i++) {
            AstNode *method = cls->methods[i];
            if (!method || method->type != AST_METHOD_DECL)
                continue;
            MethodDeclNode *md = &method->as.method_decl;
            if (!md->body)
                continue;
            XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
            XaSymbolLinks *method_links =
                method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
            if (!method_links)
                continue;
            if (xa_symbol_links_set_param_escape_summary(
                    ctx, method_links, method_links->param_types, method_links->param_names,
                    method_links->param_count, method_links->return_type, md->body, info)) {
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    xa_analyzer_exit_scope(ctx->analyzer);

    // After all fields/methods are collected, enforce that every user-defined
    // interface listed in `implements` is structurally satisfied by this
    // class. Built-in interfaces (Iterable, Comparable, ...) are validated
    // separately when used as generic constraints.
    xa_check_interface_conformance(ctx, node, info);
}

void xa_visit_collect_var_decl(XaInferContext *ctx, AstNode *node) {
    XR_DCHECK(ctx != NULL, "visit_collect_var_decl: NULL ctx");
    if (!node)
        return;

    VarDeclNode *var = &node->as.var_decl;
    bool is_const = node->type == AST_CONST_DECL;
    var->is_const = is_const;

    XaSymbol *sym =
        var->symbol_id ? xa_scope_lookup_by_id(ctx->analyzer->global_scope, var->symbol_id) : NULL;
    if (!sym) {
        sym = xa_symbol_new(var->name, XA_SYM_VARIABLE);
        sym->location.line = node->line;
        sym->is_const = is_const;
        sym->is_exported = node->is_exported;
        sym->is_readonly_binding = is_const;
        sym->is_rebindable = !is_const;

        xa_visit_add_symbol_checked(ctx, sym, 0);

        /* Write back unique symbol ID so Xi lowering can use it as Braun SSA key
         * instead of name-based lookup (eliminates scope ambiguity). */
        var->symbol_id = sym->id;
    } else {
        sym->is_const = is_const;
        sym->is_exported = node->is_exported;
        sym->is_readonly_binding = is_const;
        sym->is_rebindable = !is_const;
    }

    // Type will be inferred in pass 2
    // Keep NULL when no annotation (distinguishes "no annotation" from "annotated as any")
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        links->binding_use = XA_BINDING_UNINITIALIZED;
        links->root_id = sym->id;
        links->root_alias = XA_ROOT_UNIQUE;
        links->binding_mutability = is_const ? XA_BINDING_STABLE : XA_BINDING_REBINDABLE;
        links->value_capability = is_const ? XA_CAP_CONST : XA_CAP_MUTABLE;
        links->storage_domain =
            ctx->analyzer->current_scope && ctx->analyzer->current_scope->kind == XA_SCOPE_GLOBAL
                ? XR_STORAGE_MODULE_STATIC
                : XR_STORAGE_EXEC_LOCAL;
        links->const_initializer = sym->is_const ? var->initializer : NULL;
    }
    links->declared_type = var->type_annotation
                               ? xr_tref_resolve_in_analyzer(ctx->analyzer, var->type_annotation)
                               : NULL;
    if (links->declared_type) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_validate_hashable_key_type(ctx, links->declared_type, NULL, "type annotation", &loc);
        xa_check_weak_storage_domain(ctx, links->declared_type, links->storage_domain, &loc);
    }

    /* Qualify the declaration through the canonical type constructor. Never
     * mutate a pooled type in place: doing so would silently turn every use of
     * the same enum/class type const across the analyzer session. */
    if (sym->is_const && links->declared_type)
        links->declared_type = xr_type_make_const(ctx->analyzer->isolate, links->declared_type);

    // Recurse into inline go-lambda calls to collect nested scopes. Their body
    // needs Pass 1 scope collection for for-in variables, multi-value decls, etc.
    // Must mirror xa_visit_function_body_unified exactly: function scope,
    // then the body statements inline. Function bodies deliberately do not
    // add a second block scope; doing so strands the Pass 1 symbols in a child
    // scope that Pass 2 never enters.
    AstNode *init = var->initializer;
    if (init && init->type == AST_GO_EXPR) {
        AstNode *go_call = init->as.go_expr.expr;
        AstNode *go_fn =
            go_call && go_call->type == AST_CALL_EXPR ? go_call->as.call_expr.callee : NULL;
        if (go_fn && go_fn->type == AST_FUNCTION_EXPR) {
            FunctionDeclNode *fn = &go_fn->as.function_expr;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, go_fn);
            if (fn->body)
                xa_visit_collect(ctx, fn->body);
            xa_analyzer_exit_scope(ctx->analyzer);
        }
    }
}

/* ============================================================================
 * Pass 1.5: Link Class Inheritance
 * ============================================================================
 * After Pass 1 collects all class symbols, this pass links inheritance chains
 * by resolving base class names to actual XrClassInfo pointers.
 * ========================================================================== */

static bool method_types_equal_for_override(XaSymbol *method, XaSymbol *parent_method) {
    if (!method || !parent_method)
        return false;
    XrType *method_type = method->links.type;
    XrType *parent_type = parent_method->links.type;
    if (!method_type || !parent_type)
        return false;
    if (method_type->kind != XR_KIND_FUNCTION || parent_type->kind != XR_KIND_FUNCTION)
        return false;
    return xr_type_function_signature_assignable(parent_type, method_type);
}

static XaSymbol *find_parent_override_target(XrClassInfo *info, XaSymbol *method,
                                             bool *out_name_match) {
    if (out_name_match)
        *out_name_match = false;
    if (!info || !method || !method->name)
        return NULL;

    for (XrClassInfo *base = info->base; base; base = base->base) {
        for (int i = 0; i < base->method_count; i++) {
            XaSymbol *candidate = base->methods[i];
            if (!candidate || !candidate->name || candidate->is_static || candidate->is_private)
                continue;
            if (strcmp(candidate->name, method->name) != 0)
                continue;
            if (out_name_match)
                *out_name_match = true;
            if (method_types_equal_for_override(method, candidate))
                return candidate;
        }
    }
    return NULL;
}

static XaSymbol *find_parent_method_by_name(XrClassInfo *info, const char *name,
                                            bool include_static) {
    if (!info || !name)
        return NULL;
    for (XrClassInfo *base = info->base; base; base = base->base) {
        for (int i = 0; i < base->method_count; i++) {
            XaSymbol *candidate = base->methods[i];
            if (candidate && candidate->name && !candidate->is_private &&
                strcmp(candidate->name, name) == 0)
                return candidate;
        }
        if (include_static) {
            for (int i = 0; i < base->static_method_count; i++) {
                XaSymbol *candidate = base->static_methods[i];
                if (candidate && candidate->name && !candidate->is_private &&
                    strcmp(candidate->name, name) == 0)
                    return candidate;
            }
        }
    }
    return NULL;
}

static XaSymbol *find_parent_field_by_name(XrClassInfo *info, const char *name) {
    if (!info || !name)
        return NULL;
    for (XrClassInfo *base = info->base; base; base = base->base) {
        for (int i = 0; i < base->field_count; i++) {
            XaSymbol *candidate = base->fields[i];
            if (candidate && candidate->name && !candidate->is_private &&
                strcmp(candidate->name, name) == 0)
                return candidate;
        }
    }
    return NULL;
}

static XrClassInfo *class_info_from_type(XrType *type) {
    if (!type)
        return NULL;
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;
    return type->instance.class_ref;
}

static int xa_class_symbol_count_recursive(XaScope *scope) {
    if (!scope)
        return 0;

    int total = 0;
    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (sym && sym->kind == XA_SYM_CLASS)
            total++;
    }
    if (symbols)
        xr_free(symbols);

    for (int i = 0; i < scope->child_count; i++)
        total += xa_class_symbol_count_recursive(scope->children ? scope->children[i] : NULL);

    return total;
}

static int xa_collect_class_symbols_recursive(XaScope *scope, XaSymbol **out, int max) {
    if (!scope || !out || max <= 0)
        return 0;

    int written = 0;
    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);
    for (int i = 0; i < count && written < max; i++) {
        XaSymbol *sym = symbols[i];
        if (sym && sym->kind == XA_SYM_CLASS)
            out[written++] = sym;
    }
    if (symbols)
        xr_free(symbols);

    for (int i = 0; i < scope->child_count && written < max; i++) {
        written += xa_collect_class_symbols_recursive(scope->children ? scope->children[i] : NULL,
                                                      out + written, max - written);
    }
    return written;
}

static XrClassInfo *resolve_base_class_info(XaAnalyzer *analyzer, XaScope *scope,
                                            const char *base_name, XrType **out_base_type) {
    if (out_base_type)
        *out_base_type = NULL;
    if (!analyzer || !base_name)
        return NULL;

    XaSymbol *base_sym = scope ? xa_scope_lookup(scope, base_name) : NULL;
    if (!base_sym)
        base_sym = xa_scope_lookup(analyzer->global_scope, base_name);
    if (!base_sym)
        return NULL;

    XaSymbolLinks *base_links = xa_analyzer_get_links(analyzer, base_sym);
    if (!base_links)
        return NULL;

    if (out_base_type)
        *out_base_type = base_links->type;

    if (base_sym->kind == XA_SYM_CLASS && base_links->class_info)
        return base_links->class_info;

    return class_info_from_type(base_links->type);
}

static void report_method_hiding_conflict(XaAnalyzer *analyzer, XrClassInfo *info, XaSymbol *method,
                                          XaSymbol *parent_method) {
    if (!analyzer || !method)
        return;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "method '%s.%s' conflicts with inherited method '%s'; "
             "Xray does not support method overload or method hiding",
             info && info->name ? info->name : "?", method->name ? method->name : "?",
             parent_method && parent_method->name ? parent_method->name : "?");
    XrLocation loc = method->location;
    if (!loc.file)
        loc.file = analyzer->current_file;
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_OVERRIDE_MISMATCH, msg,
                               &loc);
}

static void report_inherited_member_conflict(XaAnalyzer *analyzer, XrClassInfo *info,
                                             XaSymbol *member, const char *member_kind,
                                             XaSymbol *parent_member, const char *parent_kind) {
    if (!analyzer || !member)
        return;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s '%s.%s' conflicts with inherited %s '%s'; "
             "Xray does not support member hiding",
             member_kind ? member_kind : "member", info && info->name ? info->name : "?",
             member->name ? member->name : "?", parent_kind ? parent_kind : "member",
             parent_member && parent_member->name ? parent_member->name : "?");
    XrLocation loc = member->location;
    if (!loc.file)
        loc.file = analyzer->current_file;
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_OVERRIDE_MISMATCH, msg,
                               &loc);
}

static bool symbol_has_param_defaults(XaSymbol *sym) {
    if (!sym)
        return false;
    XaSymbolLinks *links = &sym->links;
    if (!links->param_defaults || links->param_count <= 0)
        return false;
    for (int i = 0; i < links->param_count; i++) {
        if (links->param_defaults[i])
            return true;
    }
    return false;
}

static void report_default_arg_override_conflict(XaAnalyzer *analyzer, XrClassInfo *info,
                                                 XaSymbol *method, XaSymbol *parent_method) {
    if (!analyzer || !method)
        return;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "override method '%s.%s' cannot redefine default arguments for slot '%s.%s'",
             info && info->name ? info->name : "?", method->name ? method->name : "?",
             parent_method && parent_method->parent && parent_method->parent->name
                 ? parent_method->parent->name
                 : "?",
             parent_method && parent_method->name ? parent_method->name : "?");
    XrLocation loc = method->location;
    if (!loc.file)
        loc.file = analyzer->current_file;
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_OVERRIDE_MISMATCH, msg,
                               &loc);
}

static void validate_method_override_graph(XaAnalyzer *analyzer, XrClassInfo *info) {
    if (!analyzer || !info)
        return;

    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *field = info->fields[i];
        if (!field || !field->name)
            continue;
        XaSymbol *parent_field = find_parent_field_by_name(info, field->name);
        if (parent_field)
            report_inherited_member_conflict(analyzer, info, field, "field", parent_field, "field");
        XaSymbol *parent_method = find_parent_method_by_name(info, field->name, true);
        if (parent_method)
            report_inherited_member_conflict(analyzer, info, field, "field", parent_method,
                                             "method");
    }

    for (int i = 0; i < info->static_method_count; i++) {
        XaSymbol *method = info->static_methods[i];
        if (!method || !method->name)
            continue;
        XaSymbol *parent_field = find_parent_field_by_name(info, method->name);
        if (parent_field)
            report_inherited_member_conflict(analyzer, info, method, "static method", parent_field,
                                             "field");
        XaSymbol *parent = find_parent_method_by_name(info, method->name, true);
        if (parent)
            report_method_hiding_conflict(analyzer, info, method, parent);
    }

    for (int i = 0; i < info->method_count; i++) {
        XaSymbol *method = info->methods[i];
        if (!method || !method->name)
            continue;
        if (strcmp(method->name, "constructor") == 0)
            continue;
        if (!info->base)
            continue;
        XaSymbol *parent_field = find_parent_field_by_name(info, method->name);
        if (parent_field)
            report_inherited_member_conflict(analyzer, info, method, "method", parent_field,
                                             "field");
        bool name_match = false;
        XaSymbol *target = find_parent_override_target(info, method, &name_match);
        if (target) {
            if (symbol_has_param_defaults(method))
                report_default_arg_override_conflict(analyzer, info, method, target);
            method->is_override = true;
            continue;
        }
        if (name_match)
            report_method_hiding_conflict(analyzer, info, method,
                                          find_parent_method_by_name(info, method->name, false));
    }
}

static int32_t xa_method_runtime_symbol(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !analyzer->isolate || !name)
        return 0;
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(analyzer->isolate);
    SymbolId id = xr_symbol_register_in_table(table, name);
    return id == SYMBOL_INVALID ? 0 : (int32_t) id;
}

// Build virtual method table for a class (inherits base vtable + own methods)
static void build_class_vtable(XaAnalyzer *analyzer, XrClassInfo *info) {
    if (!info || info->vtable)
        return;  // already built

    // First build base vtable if needed
    if (info->base) {
        build_class_vtable(analyzer, info->base);
    }

    // Determine vtable size: base methods + new methods
    int base_size = info->base ? info->base->vtable_size : 0;
    int max_size = base_size + info->method_count;
    if (max_size == 0)
        return;

    XaMethodSlot *vtable = xr_calloc(max_size, sizeof(XaMethodSlot));
    int vt_count = 0;

    // Copy base vtable entries (inherit)
    if (info->base && info->base->vtable) {
        for (int i = 0; i < info->base->vtable_size; i++) {
            vtable[i] = info->base->vtable[i];
            vtable[i].is_final = true;  // assume final until proven otherwise
            vt_count++;
        }
    }

    // Process own methods: override existing or add new
    for (int m = 0; m < info->method_count; m++) {
        XaSymbol *method = info->methods[m];
        if (!method || !method->name)
            continue;
        int32_t method_symbol = xa_method_runtime_symbol(analyzer, method->name);
        if (method_symbol <= 0)
            continue;

        // Check if this overrides a base method
        bool found = false;
        for (int v = 0; v < vt_count; v++) {
            if (vtable[v].symbol_id == method_symbol) {
                // Override: mark base method as overridden
                if (info->base && info->base->vtable) {
                    for (int bv = 0; bv < info->base->vtable_size; bv++) {
                        if (info->base->vtable[bv].symbol_id == method_symbol) {
                            info->base->vtable[bv].is_overridden = true;
                            info->base->vtable[bv].is_final = false;
                            break;
                        }
                    }
                }
                // Update slot to point to overriding method
                vtable[v].symbol = method;
                vtable[v].is_overridden = false;
                vtable[v].is_final = true;
                found = true;
                break;
            }
        }

        if (!found) {
            // New method, add to vtable
            vtable[vt_count].name = method->name;
            vtable[vt_count].symbol_id = method_symbol;
            vtable[vt_count].symbol = method;
            vtable[vt_count].is_overridden = false;
            vtable[vt_count].is_final = true;
            vtable[vt_count].vtable_index = vt_count;
            vt_count++;
        }
    }

    // Assign vtable indices
    for (int i = 0; i < vt_count; i++) {
        vtable[i].vtable_index = i;
    }

    info->vtable = vtable;
    info->vtable_size = vt_count;
}

void xa_link_class_inheritance(XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->global_scope)
        return;

    int count = xa_class_symbol_count_recursive(analyzer->global_scope);
    XaSymbol **symbols = count > 0 ? xr_malloc(sizeof(XaSymbol *) * (size_t) count) : NULL;
    if (!symbols)
        return;
    count = xa_collect_class_symbols_recursive(analyzer->global_scope, symbols, count);

    // Pass 1: Link all class inheritance chains.
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym)
            continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;

        /* Only the declaring module owns its class graph. Imported symbols
         * are views sharing the owner's XrClassInfo pointer; resolving the
         * base name in the importer's scope (where it may not be visible)
         * used to overwrite the owner's already-linked base with NULL and
         * sever inheritance for every module, including the owner. */
        if (!links->owns_class_info)
            continue;

        XrClassInfo *info = links->class_info;
        if (!info->base_name)
            continue;

        XrType *base_type = NULL;
        XrClassInfo *base_info =
            resolve_base_class_info(analyzer, sym->scope, info->base_name, &base_type);
        if (base_info) {
            if (base_info->explicit_final) {
                char msg[256];
                snprintf(msg, sizeof(msg), "class '%s' is final and cannot be extended",
                         base_info->name ? base_info->name : info->base_name);
                XrLocation loc = info->location;
                if (!loc.file)
                    loc.file = analyzer->current_file;
                xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg, &loc);
                info->base = NULL;
                continue;
            }
            info->base = base_info;
            base_info->has_subclass = true;
            // Link XrType inheritance chain for xr_type_is_subclass_of().
            if (links->type && base_type) {
                links->type->instance.superclass = base_type;
            }
        } else if (!info->base) {
            /* Unresolved here and never linked anywhere: leave it NULL. A
             * link already established by the owning module must survive a
             * later module's failed re-resolution. */
            info->base = NULL;
        }
    }

    // Pass 2: Validate inferred override graph now that parent links exist.
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym)
            continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info || !links->owns_class_info)
            continue;

        validate_method_override_graph(analyzer, links->class_info);
    }

    // Pass 3: Build virtual method tables (after all inheritance is linked)
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym)
            continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info || !links->owns_class_info)
            continue;

        build_class_vtable(analyzer, links->class_info);
    }

    // Pass 4: Mark methods as non-final if class has subclass
    // (A method is only truly final if no subclass exists)
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;
        XrClassInfo *info = links->class_info;
        if (!info->vtable)
            continue;

        // If class has no subclass, all its methods are definitively final
        // (is_final = true is already default)
        // If class has subclass, methods not overridden are still final
        // (handled above during vtable build)
    }

    xr_free(symbols);
}

/* ========== Cycle Candidate Detection ========== */

/* DFS states for Tarjan-style cycle detection in the class reference graph.
 * A class A references class B if any of A's instance fields has type B
 * (or a union containing B, or an array of B, etc.). If a strongly connected
 * component (SCC) of size > 1 exists, all its members are cycle candidates.
 * Self-referencing classes (A has a field of type A|null) are also candidates. */

#define CYC_UNVISITED 0
#define CYC_ON_STACK 1
#define CYC_DONE 2

/* Edges out of one field's type.
 *
 * The rule is one-sided on purpose: over-marking costs a cheap flag test,
 * MISSING a mark is a correctness hole — an unmarked class never becomes a
 * cycle candidate, so nothing downstream can see its cycles at all.
 *
 * This replaced a "return the first class name found" helper whose losses were
 * all missed marks: it stopped at the first matching union member, never
 * looked inside Map/Set (a `children: Map<string, Node>` produced no edge at
 * all), had no way to say "this field can hold any object", and matched
 * classes by name so two modules' `Node` were the same node.
 */
#define XA_CYCLE_MAX_EDGES 64
#define XA_CYCLE_TYPE_DEPTH_MAX 16

typedef struct {
    /* Concrete references. XrClassInfo is the identity where the type carries
     * one; the name is the fallback for types that only got a name. */
    XrClassInfo *edge_info[XA_CYCLE_MAX_EDGES];
    const char *edge_name[XA_CYCLE_MAX_EDGES];
    int edge_count;
    /* A field that can hold an arbitrary object yields no usable edge: Json,
     * and function/closure types (a closure captures whatever it likes). The
     * holder becomes a candidate on its own, because the graph cannot rule a
     * cycle out. Edge-table overflow sets this too. */
    bool opaque;
} CycleEdgeSet;

static void cycle_edge_add(CycleEdgeSet *set, XrClassInfo *info, const char *name) {
    if (!set || (!info && !name))
        return;
    for (int i = 0; i < set->edge_count; i++) {
        if (info && set->edge_info[i] == info)
            return;
        if (!info && !set->edge_info[i] && set->edge_name[i] && name &&
            strcmp(set->edge_name[i], name) == 0)
            return;
    }
    if (set->edge_count >= XA_CYCLE_MAX_EDGES) {
        set->opaque = true; /* out of room: fail towards marking */
        return;
    }
    set->edge_info[set->edge_count] = info;
    set->edge_name[set->edge_count] = name;
    set->edge_count++;
}

static void cycle_collect_type_edges(XaAnalyzer *analyzer, XrType *type, CycleEdgeSet *set,
                                     int depth);

/* A struct is a value type: it has no reference-count identity of its own, so
 * it can never BE a member of a cycle. It can still be a step ALONG one, so it
 * is not a graph node but a pass-through, and the edges out of its fields are
 * attributed to the class holding it. (Skipping structs outright was the old
 * behaviour; treating one as a node would be wrong in the other direction.)
 *
 * Today this is DEFENSIVE rather than load-bearing. Measured 2026-08-01, the
 * analyzer already rejects the field types that would make a struct part of a
 * cycle:
 *
 *   struct S { label: string }     accepted
 *   struct S { items: Array<int> } rejected, "cannot use a dynamic container type"
 *   struct S { parent: Node? }     rejected, "only scalar values, raw pointers,
 *                                   fixed arrays and other structs are supported"
 *
 * so `class Node { data: S }` + `struct S { parent: Node? }` cannot currently
 * be written. The pass-through is still the right shape: it costs nothing, it
 * is what keeps the walk from mistaking a struct for a node, and it starts
 * carrying real cycles the moment the field restriction is relaxed. */
static void cycle_collect_struct_edges(XaAnalyzer *analyzer, XrClassInfo *info, CycleEdgeSet *set,
                                       int depth) {
    if (!analyzer || !info || depth >= XA_CYCLE_TYPE_DEPTH_MAX) {
        if (set && depth >= XA_CYCLE_TYPE_DEPTH_MAX)
            set->opaque = true;
        return;
    }
    /* A boxed self-referential struct (`struct S { child: S? }`) would recur
     * forever without the depth bound above; unboxed self-containment is
     * impossible (infinite size) and rejected earlier. */
    for (XrClassInfo *c = info; c; c = c->base) {
        for (int f = 0; f < c->field_count; f++) {
            XaSymbol *field_sym = c->fields[f];
            if (!field_sym || field_sym->is_static)
                continue;
            XaSymbolLinks *fl = xa_analyzer_get_links(analyzer, field_sym);
            if (fl && fl->type)
                cycle_collect_type_edges(analyzer, fl->type, set, depth + 1);
        }
    }
}

static void cycle_collect_type_edges(XaAnalyzer *analyzer, XrType *type, CycleEdgeSet *set,
                                     int depth) {
    if (!type || !set)
        return;
    if (depth >= XA_CYCLE_TYPE_DEPTH_MAX) {
        set->opaque = true;
        return;
    }

    switch (type->kind) {
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE: {
            XrClassInfo *info = type->instance.class_ref;
            /* struct_layout is the discriminator, NOT the type's is_value_type
             * flag: a field whose type refers to a struct carries
             * is_value_type == 0, while the struct's own class symbol carries
             * 1. Reading the field's flag looked past every struct. */
            if (info && info->struct_layout) {
                /* Pass through the struct rather than pointing at it. */
                cycle_collect_struct_edges(analyzer, info, set, depth);
            } else {
                cycle_edge_add(set, info, type->instance.class_name);
            }
            /* Generic arguments are reachable through the instance's fields. */
            for (int i = 0; i < type->instance.type_arg_count; i++)
                cycle_collect_type_edges(analyzer, type->instance.type_args[i], set, depth + 1);
            return;
        }
        case XR_KIND_UNION:
            /* EVERY member, not the first one that happens to be a class. */
            for (int i = 0; i < type->union_type.member_count; i++)
                cycle_collect_type_edges(analyzer, type->union_type.members[i], set, depth + 1);
            return;
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
            cycle_collect_type_edges(analyzer, type->container.element_type, set, depth + 1);
            return;
        case XR_KIND_FIXED_ARRAY:
            cycle_collect_type_edges(analyzer, type->fixed_array.element_type, set, depth + 1);
            return;
        case XR_KIND_MAP:
            cycle_collect_type_edges(analyzer, type->map.key_type, set, depth + 1);
            cycle_collect_type_edges(analyzer, type->map.value_type, set, depth + 1);
            return;
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++)
                cycle_collect_type_edges(analyzer, type->tuple.element_types[i], set, depth + 1);
            return;
        case XR_KIND_JSON:
        case XR_KIND_FUNCTION:
            /* Holds anything: a Json slot takes any object, a closure captures
             * whatever it was built over. No edge is derivable, so the holder
             * is a candidate unconditionally. */
            set->opaque = true;
            return;
        case XR_KIND_TYPE_PARAM:
            /* Erased here; the monomorphic instance carries the real type. Its
             * constraint is the only thing visible, and a constrained type
             * parameter can still be bound to a cycle participant. */
            set->opaque = true;
            return;
        default:
            return;
    }
}

/* Which node in class_syms does this edge point at?
 *
 * XrClassInfo identity first: matching by name alone made two modules' `Node`
 * the same node, which could mark the wrong class and miss the right one. The
 * name is only consulted for a type that carries no class_ref, and then EVERY
 * same-named node is reported rather than the first — an ambiguous name must
 * over-mark, never pick one and stop. Returns the count reported. */
static int cycle_resolve_edge(XaAnalyzer *analyzer, XaSymbol **class_syms, int count,
                              XrClassInfo *want_info, const char *want_name, int *out,
                              int out_cap) {
    int n = 0;
    for (int j = 0; j < count && n < out_cap; j++) {
        if (!class_syms[j])
            continue;
        XaSymbolLinks *jl = xa_analyzer_get_links(analyzer, class_syms[j]);
        XrClassInfo *jinfo = jl ? jl->class_info : NULL;
        if (want_info) {
            if (jinfo == want_info)
                out[n++] = j;
            continue;
        }
        if (want_name && class_syms[j]->name && strcmp(class_syms[j]->name, want_name) == 0)
            out[n++] = j;
    }
    return n;
}

/* Recursive DFS marking. Returns true if any node in the subtree is on-stack
 * (i.e., a cycle was found). */
static bool cycle_dfs(XaAnalyzer *analyzer, XaSymbol **class_syms, uint8_t *state, int idx,
                      int count, bool *is_candidate) {
    state[idx] = CYC_ON_STACK;
    bool found_cycle = false;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, class_syms[idx]);
    if (!links || !links->class_info)
        goto done;

    XrClassInfo *info = links->class_info;

    CycleEdgeSet set = {0};
    /* Inherited fields are not in info->fields, so walk the base chain: a
     * cycle through a field declared on a superclass is still a cycle. */
    for (XrClassInfo *c = info; c; c = c->base) {
        for (int f = 0; f < c->field_count; f++) {
            XaSymbol *field_sym = c->fields[f];
            if (!field_sym || field_sym->is_static)
                continue;
            XaSymbolLinks *fl = xa_analyzer_get_links(analyzer, field_sym);
            if (!fl || !fl->type)
                continue;
            /* This is the L0/L1 interface: a `weak` field does not keep its
             * target alive, so it cannot close a cycle and must not produce an
             * edge. Annotating one is exactly how a user takes their class out
             * of the candidate set. */
            if (field_sym->is_weak)
                continue;
            cycle_collect_type_edges(analyzer, fl->type, &set, 0);
        }
    }

    /* A field that can hold any object: no edge is derivable, so this class is
     * a candidate on its own. */
    if (set.opaque) {
        is_candidate[idx] = true;
        found_cycle = true;
    }

    for (int e = 0; e < set.edge_count; e++) {
        int hits[XA_CYCLE_MAX_EDGES];
        int nhits = cycle_resolve_edge(analyzer, class_syms, count, set.edge_info[e],
                                       set.edge_name[e], hits, XA_CYCLE_MAX_EDGES);
        for (int h = 0; h < nhits; h++) {
            int j = hits[h];
            if (j == idx) {
                /* Self-reference. `class Node { children: Array<Node> }` is a
                 * tree, but the TYPE graph has a self-loop and L0 is a
                 * type-level approximation — it cannot and must not try to
                 * tell a downward edge from an upward one. */
                is_candidate[idx] = true;
                found_cycle = true;
                continue;
            }
            if (state[j] == CYC_ON_STACK) {
                /* Back edge: cycle found. Mark both. */
                is_candidate[idx] = true;
                is_candidate[j] = true;
                found_cycle = true;
            } else if (state[j] == CYC_UNVISITED) {
                if (cycle_dfs(analyzer, class_syms, state, j, count, is_candidate)) {
                    is_candidate[idx] = true;
                    found_cycle = true;
                }
            } else if (is_candidate[j]) {
                /* j already processed and is a cycle candidate — if we
                 * reference it, we are also part of a potential cycle. */
                is_candidate[idx] = true;
                found_cycle = true;
            }
        }
    }

done:
    state[idx] = CYC_DONE;
    return found_cycle;
}

void xa_mark_cycle_candidates(XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->global_scope)
        return;

    int sym_count = xa_class_symbol_count_recursive(analyzer->global_scope);
    XaSymbol **all_syms = sym_count > 0 ? xr_malloc(sizeof(XaSymbol *) * (size_t) sym_count) : NULL;
    if (!all_syms)
        return;
    sym_count = xa_collect_class_symbols_recursive(analyzer->global_scope, all_syms, sym_count);
    if (sym_count == 0) {
        xr_free(all_syms);
        return;
    }

    /* Collect only class symbols (skip structs — value types are copied). */
    int class_count = 0;
    XaSymbol **class_syms = xr_malloc(sizeof(XaSymbol *) * sym_count);
    if (!class_syms) {
        xr_free(all_syms);
        return;
    }

    for (int i = 0; i < sym_count; i++) {
        XaSymbol *sym = all_syms[i];
        if (!sym || sym->kind != XA_SYM_CLASS)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;
        if (links->type && links->type->is_value_type)
            continue; /* Skip structs. */
        class_syms[class_count++] = sym;
    }

    if (class_count == 0) {
        xr_free(class_syms);
        xr_free(all_syms);
        return;
    }

    uint8_t *state = xr_calloc(class_count, sizeof(uint8_t));
    bool *is_candidate = xr_calloc(class_count, sizeof(bool));
    if (!state || !is_candidate) {
        xr_free(state);
        xr_free(is_candidate);
        xr_free(class_syms);
        xr_free(all_syms);
        return;
    }

    /* Run DFS from each unvisited class. */
    for (int i = 0; i < class_count; i++) {
        if (state[i] == CYC_UNVISITED)
            cycle_dfs(analyzer, class_syms, state, i, class_count, is_candidate);
    }

    /* Mark XrType for cycle candidates. The lowerer propagates this to
     * XiClassData, then the emitter sets XR_CLASS_CYCLE_CANDIDATE in the
     * class descriptor flags, which the runtime class builder propagates
     * to XrClass.flags — enabling the RC cycle collector at instance alloc. */
    for (int i = 0; i < class_count; i++) {
        if (!is_candidate[i])
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, class_syms[i]);
        if (links && links->type)
            links->type->is_cycle_candidate = true;
    }

    xr_free(state);
    xr_free(is_candidate);
    xr_free(class_syms);
    xr_free(all_syms);
}
