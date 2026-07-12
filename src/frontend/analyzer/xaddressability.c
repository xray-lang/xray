/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xaddressability.h"
#include "xanalyzer_visitor_internal.h"

static XaSymbol *address_symbol(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE)
        return NULL;
    uint32_t symbol_id = expr->as.variable.symbol_id;
    if (symbol_id != 0) {
        XaSymbol *symbol = xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id);
        if (symbol)
            return symbol;
    }
    return expr->as.variable.name ? xa_lookup_visible_symbol(ctx, expr->as.variable.name) : NULL;
}

static XrType *address_expr_type(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr)
        return NULL;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    return type ? type : xa_visit_infer_expr(ctx, expr);
}

static bool address_type_has_native_layout(XrType *type) {
    return xa_type_supports_const_static_data_object(type) ||
           xr_type_has_static_layout(type, NULL, NULL);
}

static XaAddressability address_none(XaAddressRejectReason rejection) {
    XaAddressability result = {0};
    result.rejection = rejection;
    return result;
}

static XaAddressability classify_variable(XaInferContext *ctx, AstNode *expr, bool wants_mutable) {
    XaSymbol *symbol = address_symbol(ctx, expr);
    if (!symbol || (symbol->kind != XA_SYM_VARIABLE && symbol->kind != XA_SYM_PARAMETER))
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);

    XaAddressability result = {0};
    result.base_symbol = symbol;
    result.pointee_type = xa_analyzer_get_type(ctx->analyzer, symbol);
    if (!result.pointee_type)
        result.pointee_type = address_expr_type(ctx, expr);
    result.native_layout_ok = address_type_has_native_layout(result.pointee_type);
    result.mutable_ok = !symbol->is_const && !symbol->is_readonly_binding;
    result.is_imported = symbol->is_imported;
    result.is_shared = symbol->is_shared;

    if (symbol->scope && symbol->scope->kind == XA_SCOPE_GLOBAL) {
        result.kind = XA_ADDRESS_MODULE_STATIC;
        result.lifetime = XA_ADDRESS_LIFETIME_MODULE;
        result.storage_owner = symbol->is_shared ? XR_STORAGE_SHARED_SYSTEM : XR_STORAGE_MODULE;
        result.address_identity =
            symbol->is_shared ? XR_ADDRESS_SHARED_STABLE : XR_ADDRESS_MODULE_STABLE;
    } else if (symbol->kind == XA_SYM_PARAMETER) {
        result.kind = XA_ADDRESS_PARAMETER;
        result.lifetime = XA_ADDRESS_LIFETIME_CALL;
        result.storage_owner = XR_STORAGE_EXEC_LOCAL;
        result.address_identity = XR_ADDRESS_LEXICAL;
    } else {
        result.kind = XA_ADDRESS_STACK_LOCAL;
        result.lifetime = XA_ADDRESS_LIFETIME_LEXICAL;
        result.storage_owner = XR_STORAGE_EXEC_LOCAL;
        result.address_identity = XR_ADDRESS_LEXICAL;
    }

    if (!result.native_layout_ok)
        result.rejection = XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT;
    else if (wants_mutable && !result.mutable_ok)
        result.rejection = XA_ADDRESS_REJECT_READONLY;
    else if (result.kind == XA_ADDRESS_STACK_LOCAL || result.kind == XA_ADDRESS_PARAMETER)
        result.rejection = XA_ADDRESS_REJECT_ESCAPE_UNPROVEN;
    else
        result.rejection = XA_ADDRESS_OK;
    return result;
}

static XaAddressability classify_member(XaInferContext *ctx, AstNode *expr, bool wants_mutable) {
    MemberAccessNode *member = &expr->as.member_access;
    if (!member->object || !member->name)
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);
    XaAddressability base = xa_classify_addressability(ctx, member->object, wants_mutable);
    XaAddressability result = base;
    result.kind = XA_ADDRESS_FIELD;
    result.pointee_type = address_expr_type(ctx, expr);
    result.native_layout_ok = address_type_has_native_layout(result.pointee_type);
    if (!result.native_layout_ok) {
        result.rejection = XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT;
        return result;
    }
    XrType *base_type = address_expr_type(ctx, member->object);
    if (!base_type ||
        !xr_type_has_static_field_offset(base_type, member->name, &result.field_offset)) {
        result.rejection = XA_ADDRESS_REJECT_STORAGE_MAY_MOVE;
        return result;
    }
    if (wants_mutable && !result.mutable_ok) {
        result.rejection = XA_ADDRESS_REJECT_READONLY;
        return result;
    }
    if (base.rejection != XA_ADDRESS_OK)
        result.rejection = base.rejection;
    return result;
}

static XaAddressability classify_index(XaInferContext *ctx, AstNode *expr, bool wants_mutable) {
    IndexGetNode *index = &expr->as.index_get;
    if (!index->array)
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);
    XaAddressability base = xa_classify_addressability(ctx, index->array, wants_mutable);
    XaAddressability result = base;
    XrType *owner_type = address_expr_type(ctx, index->array);
    result.pointee_type = address_expr_type(ctx, expr);
    result.native_layout_ok = address_type_has_native_layout(result.pointee_type);
    if (owner_type && owner_type->kind == XR_KIND_FIXED_ARRAY) {
        result.kind = XA_ADDRESS_FIXED_ARRAY_ELEMENT;
        if (!result.native_layout_ok)
            result.rejection = XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT;
        else if (wants_mutable && !result.mutable_ok)
            result.rejection = XA_ADDRESS_REJECT_READONLY;
        else if (base.rejection != XA_ADDRESS_OK)
            result.rejection = base.rejection;
        return result;
    }
    result.kind = XA_ADDRESS_OWNER_ELEMENT;
    result.lifetime = XA_ADDRESS_LIFETIME_OWNER;
    result.address_identity = XR_ADDRESS_NONE;
    result.rejection = XA_ADDRESS_REJECT_DYNAMIC_OWNER_ELEMENT;
    return result;
}

XaAddressability xa_classify_addressability(XaInferContext *ctx, AstNode *expr,
                                            bool wants_mutable) {
    if (!expr)
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);
    switch (expr->type) {
        case AST_VARIABLE:
            return classify_variable(ctx, expr, wants_mutable);
        case AST_MEMBER_ACCESS:
            return classify_member(ctx, expr, wants_mutable);
        case AST_INDEX_GET:
            return classify_index(ctx, expr, wants_mutable);
        case AST_GROUPING:
            return xa_classify_addressability(ctx, expr->as.grouping, wants_mutable);
        case AST_CALL_EXPR:
        case AST_ARRAY_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_STRUCT_LITERAL:
            return address_none(XA_ADDRESS_REJECT_TEMPORARY);
        default:
            /* The classifier is called for expressions. Any other expression
             * shape produces a temporary value rather than an addressable
             * storage location. */
            return address_none(XA_ADDRESS_REJECT_TEMPORARY);
    }
}

const char *xa_address_kind_name(XaAddressKind kind) {
    static const char *names[] = {
        "none",  "module static",       "stack local",   "parameter",
        "field", "fixed-array element", "owner element",
    };
    return (unsigned) kind < sizeof(names) / sizeof(names[0]) ? names[kind]
                                                              : "invalid address kind";
}

const char *xa_address_reject_reason_name(XaAddressRejectReason reason) {
    static const char *names[] = {
        "addressable",      "not an lvalue",    "temporary expression",  "no native layout",
        "storage may move", "readonly storage", "dynamic owner element", "escape cannot be proven",
    };
    return (unsigned) reason < sizeof(names) / sizeof(names[0]) ? names[reason]
                                                                : "invalid addressability";
}
